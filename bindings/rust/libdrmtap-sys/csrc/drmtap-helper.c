/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file drmtap-helper.c
 * @brief Privileged helper binary — opens DRM with CAP_SYS_ADMIN and passes
 *        file descriptors to the unprivileged library via SCM_RIGHTS.
 *
 * Installation:
 *   sudo cp drmtap-helper /usr/local/bin/drmtap-helper
 *   sudo setcap cap_sys_admin+ep /usr/local/bin/drmtap-helper
 *
 * Protocol:
 *   - Helper inherits a Unix socket on fd 3 (HELPER_SOCKET_FD)
 *   - Receives requests: 1 byte command
 *   - Sends responses: 1 byte status + optional DMA-BUF fd via SCM_RIGHTS
 *
 * Commands:
 *   CMD_GRAB (0x01): Open DRM, get FB2, export as DMA-BUF, send fd
 *   CMD_QUIT (0xFF): Clean exit
 *
 * Security:
 *   - Validates socket fd on startup (refuses to run if not spawned by the lib)
 *   - Restricts the opened device to a path that canonicalizes under /dev/dri/
 *   - Sets PR_SET_NO_NEW_PRIVS
 *   - Drops all caps except CAP_SYS_ADMIN via libcap (hard-fail if unavailable)
 *   - Installs a default-KILL seccomp allowlist (hard-fail if unavailable)
 *   - Sets FD_CLOEXEC on all opened fds
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/un.h>
#include "wire.h"
#include <sys/prctl.h>
#include <limits.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>  /* DRM_FORMAT_MOD_INVALID */

#ifdef HAVE_LIBCAP
#include <sys/capability.h>
#endif

#ifdef HAVE_SECCOMP
#include <seccomp.h>
#endif

/* virtio-gpu transfer support — needed to pull pixels from host GPU to guest RAM */
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/virtio_gpu.h>
#ifndef DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST
struct drm_virtgpu_3d_transfer_from_host {
    __u32 bo_handle;
    __u32 pad;
    __u64 offset;
    __u32 level;
    __u32 stride;
    __u32 layer_stride;
    struct drm_virtgpu_3d_box {
        __u32 x, y, z, w, h, d;
    } box;
};
struct drm_virtgpu_3d_wait {
    __u32 handle;
    __u32 flags;
};
#define DRM_VIRTGPU_TRANSFER_FROM_HOST 0x07
#define DRM_VIRTGPU_WAIT              0x08
#define DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_TRANSFER_FROM_HOST, \
             struct drm_virtgpu_3d_transfer_from_host)
#define DRM_IOCTL_VIRTGPU_WAIT \
    DRM_IOW(DRM_COMMAND_BASE + DRM_VIRTGPU_WAIT, \
            struct drm_virtgpu_3d_wait)
#endif
/* GETPARAM (used to detect virgl 3D) lives in <drm/virtgpu_drm.h>, not in
 * <linux/virtio_gpu.h>; provide the uapi definition if it's missing. */
#ifndef DRM_IOCTL_VIRTGPU_GETPARAM
struct drm_virtgpu_getparam {
    __u64 param;
    __u64 value;
};
#define VIRTGPU_PARAM_3D_FEATURES 1
#define DRM_VIRTGPU_GETPARAM 0x03
#define DRM_IOCTL_VIRTGPU_GETPARAM \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_GETPARAM, \
             struct drm_virtgpu_getparam)
#endif
#endif

/* Socket fd inherited from parent (via socketpair) */
#define HELPER_SOCKET_FD 3

/* Protocol commands (CMD_*), the command frame (helper_cmd_grab_t) and its
 * magic/version validation live in wire.h, shared with the library client so
 * the two ends cannot drift. */

/* Response status */
#define RESP_OK    0x00
#define RESP_ERROR 0x01

/* Metadata sent before pixel data — must match helper_grab_result_t in library */
#define FLAG_HAS_DMABUF 0x01
/* The exported DMA-BUF is a host-rendered virgl scanout: the parent must read it
 * back on the GPU (EGL import + glReadPixels), not via a CPU mmap, which comes
 * back black for a host-side resource. */
#define FLAG_VIRGL      0x02

struct grab_metadata {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t fb_id;
    uint32_t data_size;
    uint64_t modifier;
    uint32_t seq;
    uint64_t timestamp_ms;
    uint32_t flags;
    uint32_t hdr_eotf;      /* DRM EOTF of the scanout: 0=SDR, 2=PQ (ST2084), 3=HLG */
    uint32_t hdr_max_nits;  /* mastering/content peak luminance (cd/m2), 0=unknown */
};

/* Send a file descriptor via SCM_RIGHTS (shared framing; see src/wire.h). */
static int send_fd(int sock, int fd, const void *meta, size_t meta_len) {
    return wire_send_fd(sock, fd, meta, meta_len);
}

/* ========================================================================= */
/* Security hardening                                                        */
/* ========================================================================= */

#ifdef HAVE_LIBCAP
// Drop all capabilities except CAP_SYS_ADMIN
static int drop_caps(void) {
    cap_t caps = cap_init();
    if (!caps) {
        perror("drmtap-helper: cap_init failed"); return -1;
    }

    cap_value_t keep[] = { CAP_SYS_ADMIN };
    if (cap_set_flag(caps, CAP_PERMITTED, 1, keep, CAP_SET) != 0 ||
        cap_set_flag(caps, CAP_EFFECTIVE, 1, keep, CAP_SET) != 0) {
        int saved = errno;       /* cap_free() may clobber errno */
        cap_free(caps);
        errno = saved;
        perror("drmtap-helper: cap_set_flag failed"); return -1;
    }

    int ret = cap_set_proc(caps);
    int saved = errno;           /* preserve before cap_free() touches errno */
    cap_free(caps);

    if (ret != 0) {
        errno = saved;
        perror("drmtap-helper: cap_set_proc failed");
        return -1;
    }
    fprintf(stderr, "drmtap-helper: dropped caps, keeping CAP_SYS_ADMIN\n");
    return 0;
}
#endif

#ifdef HAVE_SECCOMP
// Install seccomp filter allowing only needed syscalls
static int install_seccomp(void) {
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_KILL_PROCESS);
    if (!ctx) {
        /* seccomp_init does not set errno; perror would print a stale value. */
        fprintf(stderr, "drmtap-helper: seccomp_init failed\n"); return -1;
    }

    /* NOTE: open/openat are deliberately NOT allowed. The DRM device is opened
     * once in main() before this filter is installed, and the grab loop only
     * ever reuses that fd — so a compromised helper cannot open arbitrary files
     * even though it holds CAP_SYS_ADMIN. */
    int allowed[] = {
        SCMP_SYS(read), SCMP_SYS(write), SCMP_SYS(close),
        SCMP_SYS(sendto),      /* send() */
        SCMP_SYS(sendmsg),     /* sendmsg() for SCM_RIGHTS */
        SCMP_SYS(recvfrom),    /* recv() */
        SCMP_SYS(mmap), SCMP_SYS(munmap),
        SCMP_SYS(brk),         /* malloc */
        SCMP_SYS(fstat), SCMP_SYS(newfstatat),
        SCMP_SYS(fcntl),
        SCMP_SYS(exit_group), SCMP_SYS(exit),
        SCMP_SYS(rt_sigreturn),
        SCMP_SYS(clock_gettime), /* used once per frame; normally vDSO, but
                                    allow the real syscall so a vDSO fallback
                                    cannot trip the default KILL action */
    };

    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        /* seccomp_rule_add returns -errno (it does not set errno itself). */
        int rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, allowed[i], 0);
        if (rc != 0) {
            seccomp_release(ctx);
            fprintf(stderr, "drmtap-helper: seccomp_rule_add failed: %s\n",
                    strerror(-rc));
            return -1;
        }
    }

    /* ioctl: allow ONLY the DRM ioctl type ('d', bits 8-15 of the request). The
     * helper issues nothing but DRM ioctls on its single DRM fd (KMS, PRIME, GEM
     * close, dumb map, VIRTGPU), so a memory-corrupted helper cannot reach an
     * unrelated ioctl (TIOCSTI console injection, terminal control, etc.). Matching
     * the type byte rather than enumerating every DRM command keeps this robust
     * across drivers and libdrm versions without risking a KILL on a legitimate DRM
     * ioctl we forgot to list. */
    {
        int rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(ioctl), 1,
                                  SCMP_A1(SCMP_CMP_MASKED_EQ, 0xFF00u,
                                          (unsigned)'d' << 8));
        if (rc != 0) {
            seccomp_release(ctx);
            fprintf(stderr, "drmtap-helper: seccomp ioctl rule failed: %s\n",
                    strerror(-rc));
            return -1;
        }
    }

    int ret = seccomp_load(ctx);
    seccomp_release(ctx);

    if (ret != 0) {
        /* seccomp_load also returns -errno. */
        fprintf(stderr, "drmtap-helper: seccomp_load failed: %s\n",
                strerror(-ret));
        return -1;
    }
    fprintf(stderr, "drmtap-helper: seccomp filter installed\n");
    return 0;
}
#endif

/* ========================================================================= */
/* Socket helpers                                                            */
/* ========================================================================= */

// Exact-length framing lives in src/wire.h so the helper, the library client,
// and the wire-protocol tests share one implementation. These thin wrappers keep
// the existing call sites unchanged.
static int send_all(int sock, const void *buf, size_t len) {
    return wire_send_all(sock, buf, len);
}
static int recv_all(int sock, void *buf, size_t len) {
    return wire_recv_all(sock, buf, len);
}

/* drmModeGetFB2 mints a fresh GEM handle that the caller owns; if it is not
 * closed, this long-running privileged helper leaks one kernel handle (and pins
 * the underlying BO) on every grab and every cursor poll. Close it on every path.
 * Harmless for handle 0. The exported DMA-BUF fd keeps its own BO reference, so
 * closing the handle after PRIME export is safe. */
static void helper_gem_close(int drm_fd, uint32_t handle) {
    if (handle == 0) {
        return;
    }
    struct drm_gem_close gc = { .handle = handle };
    drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gc);
}

// Send error: metadata with data_size=0. For logical/validation failures where
// errno carries no useful information about the cause.
static int send_error(int sock, const char *reason) {
    fprintf(stderr, "drmtap-helper: %s\n", reason);
    struct grab_metadata meta = {0};  /* data_size=0 signals error */
    send_all(sock, &meta, sizeof(meta));
    return -1;
}

// Like send_error, but for failures of a syscall or libdrm call that just set
// errno: capture it first (before send_all can overwrite it) and append it.
static int send_error_errno(int sock, const char *reason) {
    int saved = errno;
    fprintf(stderr, "drmtap-helper: %s: %s\n", reason, strerror(saved));
    struct grab_metadata meta = {0};  /* data_size=0 signals error */
    send_all(sock, &meta, sizeof(meta));
    return -1;
}

/* Cursor metadata sent before the cursor pixels — must match
 * helper_cursor_wire_t in the library (drmtap_internal.h). */
struct cursor_metadata {
    int32_t  x, y;          /* cursor plane CRTC_X/CRTC_Y (top-left on screen) */
    int32_t  hot_x, hot_y;  /* hotspot offset within the cursor image */
    uint32_t width, height; /* cursor image size */
    uint32_t visible;       /* 1 = a hardware cursor plane is active on the CRTC */
    uint32_t data_size;     /* width*height*4 if visible, else 0 */
};

// Read a single uint64 plane/object property by name (0 if absent).
static uint64_t get_prop_val(int drm_fd, uint32_t obj_id, uint32_t obj_type,
                             const char *name) {
    uint64_t val = 0;
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(drm_fd, obj_id, obj_type);
    if (!props) return 0;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(drm_fd, props->props[i]);
        if (prop) {
            if (strcmp(prop->name, name) == 0) {
                val = props->prop_values[i];
                drmModeFreeProperty(prop);
                break;
            }
            drmModeFreeProperty(prop);
        }
    }
    drmModeFreeObjectProperties(props);
    return val;
}

/* ========================================================================= */
/* Cursor capture (privileged) — reads the hardware cursor plane             */
/* ========================================================================= */

// Capture the hardware cursor plane bound to target_crtc and send its image.
// If no cursor plane is active on that CRTC (cursor idle / on another monitor),
// sends metadata with visible=0 and no pixels.
static int cursor_and_send(int sock, int drm_fd, uint32_t target_crtc) {
    struct cursor_metadata meta = {0};

    drmModePlaneRes *planes = drmModeGetPlaneResources(drm_fd);
    if (!planes) {
        send_all(sock, &meta, sizeof(meta));
        return 0;
    }

    uint32_t cursor_fb = 0, cursor_plane = 0;
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *plane = drmModeGetPlane(drm_fd, planes->planes[i]);
        if (!plane) continue;
        if (plane->fb_id != 0 &&
            (target_crtc == 0 || plane->crtc_id == target_crtc)) {
            if (get_prop_val(drm_fd, plane->plane_id, DRM_MODE_OBJECT_PLANE,
                             "type") == DRM_PLANE_TYPE_CURSOR) {
                cursor_fb = plane->fb_id;
                cursor_plane = plane->plane_id;
                drmModeFreePlane(plane);
                break;
            }
        }
        drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(planes);

    if (cursor_fb == 0) {
        send_all(sock, &meta, sizeof(meta));  /* visible=0 */
        return 0;
    }

    meta.x = (int32_t)get_prop_val(drm_fd, cursor_plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    meta.y = (int32_t)get_prop_val(drm_fd, cursor_plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    meta.hot_x = (int32_t)get_prop_val(drm_fd, cursor_plane, DRM_MODE_OBJECT_PLANE, "HOTSPOT_X");
    meta.hot_y = (int32_t)get_prop_val(drm_fd, cursor_plane, DRM_MODE_OBJECT_PLANE, "HOTSPOT_Y");

    drmModeFB2 *fb2 = drmModeGetFB2(drm_fd, cursor_fb);
    if (!fb2 || fb2->handles[0] == 0) {
        if (fb2) drmModeFreeFB2(fb2);
        send_all(sock, &meta, sizeof(meta));  /* visible=0 */
        return 0;
    }
    meta.width = fb2->width;
    meta.height = fb2->height;
    uint32_t cw = fb2->width, ch = fb2->height, cstride = fb2->pitches[0];
    uint32_t chandle = fb2->handles[0];
    drmModeFreeFB2(fb2);

    /* A hardware cursor is tiny (<= 256x256 ARGB on Linux DRM). Cap the
     * geometry so a bogus cursor fb can never make the CAP_SYS_ADMIN helper
     * mmap/send an unbounded size, and honor the source stride (pitches[0]) so
     * a padded cursor is not sent sheared. The width cap precedes width*4 so
     * that product cannot overflow. */
    if (!(cw <= 256 && ch <= 256 && cstride != 0 && cstride >= cw * 4)) {
        helper_gem_close(drm_fd, chandle);
        send_all(sock, &meta, sizeof(meta));  /* visible=0 */
        return 0;
    }

    int prime_fd = -1;
    void *mapped = MAP_FAILED;
    size_t map_size = (size_t)cstride * ch;
    if (drmPrimeHandleToFD(drm_fd, chandle, O_RDONLY | O_CLOEXEC,
                           &prime_fd) == 0 && prime_fd >= 0) {
        mapped = mmap(NULL, map_size, PROT_READ, MAP_SHARED, prime_fd, 0);
    }

    if (mapped == MAP_FAILED) {
        if (prime_fd >= 0) {
            close(prime_fd);
        }
        helper_gem_close(drm_fd, chandle);
        send_all(sock, &meta, sizeof(meta));  /* visible=0 (couldn't read) */
        return 0;
    }

    /* Repack into tightly-packed width*4 rows (what the client expects) into a
     * fixed preallocated buffer — the helper is single-threaded and keeping the
     * privileged path malloc-free avoids both allocator syscalls under seccomp
     * and heap churn. */
    static uint8_t packed[256 * 256 * 4];
    size_t tight = (size_t)cw * ch * 4;
    for (uint32_t y = 0; y < ch; y++) {
        memcpy(packed + (size_t)y * cw * 4,
               (const uint8_t *)mapped + (size_t)y * cstride,
               (size_t)cw * 4);
    }
    munmap(mapped, map_size);
    close(prime_fd);
    helper_gem_close(drm_fd, chandle);

    meta.visible = 1;
    meta.data_size = (uint32_t)tight;
    send_all(sock, &meta, sizeof(meta));
    send_all(sock, packed, tight);
    return 0;
}

/* ========================================================================= */
/* DRM capture (privileged) — reads pixels, sends via socket                 */
/* ========================================================================= */

/* Resolve the CRTC bound to a connector via the atomic CRTC_ID property. The
 * legacy encoder link (drmModeGetEncoder(conn->encoder_id)->crtc_id) reads 0 for
 * an atomically-bound / compositor-managed connector, so on Wayland an HDR display
 * never matched its CRTC and never got tone-mapped. Returns 0 if unbound. */
static uint32_t helper_connector_crtc(int drm_fd, uint32_t connector_id) {
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(drm_fd, connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (!props) {
        return 0;
    }
    uint32_t crtc = 0;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(drm_fd, props->props[i]);
        if (!p) {
            continue;
        }
        if (strcmp(p->name, "CRTC_ID") == 0) {
            crtc = (uint32_t)props->prop_values[i];
            drmModeFreeProperty(p);
            break;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return crtc;
}

/* Read the HDR transfer function + peak luminance advertised on the connector
 * driving `crtc_id`. Sets *eotf to the DRM EOTF (0 = SDR / none) and *max_nits
 * to the content/mastering peak (0 = unknown). Best-effort: any failure is
 * treated as SDR, so a missing property or a non-HDR display just means no
 * tone-mapping (the common case). */
static void read_hdr_metadata(int drm_fd, uint32_t crtc_id,
                              uint32_t *eotf, uint32_t *max_nits) {
    *eotf = 0;
    *max_nits = 0;
    if (crtc_id == 0) {
        return;
    }

    drmModeRes *res = drmModeGetResources(drm_fd);
    if (!res) {
        return;
    }
    /* Map the CRTC back to its connected connector via the atomic CRTC_ID property
     * (see helper_connector_crtc). GetConnectorCurrent reads cached kernel state
     * instead of forcing a hardware connector probe on every captured frame. */
    uint32_t conn_id = 0;
    for (int i = 0; i < res->count_connectors && conn_id == 0; i++) {
        drmModeConnector *conn =
            drmModeGetConnectorCurrent(drm_fd, res->connectors[i]);
        if (!conn) {
            continue;
        }
        if (conn->connection == DRM_MODE_CONNECTED &&
            helper_connector_crtc(drm_fd, conn->connector_id) == crtc_id) {
            conn_id = conn->connector_id;
        }
        drmModeFreeConnector(conn);
    }
    drmModeFreeResources(res);
    if (conn_id == 0) {
        return;
    }

    drmModeObjectProperties *props = drmModeObjectGetProperties(
        drm_fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
    if (!props) {
        return;
    }
    for (uint32_t p = 0; p < props->count_props; p++) {
        drmModePropertyRes *prop = drmModeGetProperty(drm_fd, props->props[p]);
        if (!prop) {
            continue;
        }
        if (strcmp(prop->name, "HDR_OUTPUT_METADATA") == 0 &&
            props->prop_values[p] != 0) {
            drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(
                drm_fd, (uint32_t)props->prop_values[p]);
#if HAVE_HDR_METADATA
            if (blob && blob->data &&
                blob->length >= sizeof(struct hdr_output_metadata)) {
                const struct hdr_output_metadata *m = blob->data;
                const struct hdr_metadata_infoframe *inf =
                    &m->hdmi_metadata_type1;
                *eotf = inf->eotf;
                /* Prefer content light level, fall back to mastering peak. */
                *max_nits = inf->max_cll ? inf->max_cll
                            : inf->max_display_mastering_luminance;
            }
#endif
            if (blob) {
                drmModeFreePropertyBlob(blob);
            }
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
}

/* A plane's 'type' (PRIMARY/OVERLAY/CURSOR) is fixed for the life of the device,
 * so probe it once and cache the primary plane ids instead of re-reading the
 * property list for every plane on every captured frame. The helper serves a single
 * persistent drm_fd, so a file-static cache is correct. */
static int plane_is_primary(int drm_fd, uint32_t plane_id) {
    static uint32_t primary_ids[32];
    static int primary_count = -1;  /* -1 = not probed yet */
    if (primary_count < 0) {
        primary_count = 0;
        drmModePlaneRes *pr = drmModeGetPlaneResources(drm_fd);
        if (pr) {
            const int cap = (int)(sizeof(primary_ids) / sizeof(primary_ids[0]));
            for (uint32_t i = 0; i < pr->count_planes && primary_count < cap; i++) {
                drmModeObjectProperties *props = drmModeObjectGetProperties(
                    drm_fd, pr->planes[i], DRM_MODE_OBJECT_PLANE);
                if (!props) {
                    continue;
                }
                for (uint32_t p = 0; p < props->count_props; p++) {
                    drmModePropertyRes *prop =
                        drmModeGetProperty(drm_fd, props->props[p]);
                    if (!prop) {
                        continue;
                    }
                    if (strcmp(prop->name, "type") == 0 &&
                        props->prop_values[p] == DRM_PLANE_TYPE_PRIMARY &&
                        primary_count < cap) {
                        primary_ids[primary_count++] = pr->planes[i];
                    }
                    drmModeFreeProperty(prop);
                }
                drmModeFreeObjectProperties(props);
            }
            drmModeFreePlaneResources(pr);
        }
    }
    for (int i = 0; i < primary_count; i++) {
        if (primary_ids[i] == plane_id) {
            return 1;
        }
    }
    return 0;
}

// Grab current framebuffer. Helper reads pixels in its own process
// (where dumb_mmap returns fresh data) and sends them via the socket.
static int grab_and_send(int sock, int drm_fd, uint32_t target_crtc, int is_virtio) {
    void *mapped = MAP_FAILED;
    size_t mapped_size = 0;

    /* Refresh plane state on persistent fd */
    drmModePlaneRes *planes = drmModeGetPlaneResources(drm_fd);
    if (!planes) {
        return send_error_errno(sock, "drmModeGetPlaneResources failed");
    }

    /* Search for the plane currently bound to the target CRTC */
    uint32_t fb_id = 0;
    uint32_t matched_crtc = 0;  /* actual CRTC of the matched plane (for HDR meta) */
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *plane = drmModeGetPlane(drm_fd, planes->planes[i]);
        if (!plane) continue;

        /* If CRTC specified, skip planes not bound to it */
        if (target_crtc != 0) {
            if (plane->crtc_id != target_crtc) {
                drmModeFreePlane(plane);
                continue;
            }
        }

        /* Check for active framebuffer */
        if (plane->fb_id != 0) {
            /* Check plane type — prefer PRIMARY (cached; the type is static). */
            int is_primary = plane_is_primary(drm_fd, plane->plane_id);

            if (is_primary || fb_id == 0) {
                fb_id = plane->fb_id;
                matched_crtc = plane->crtc_id;
                fprintf(stderr, "drmtap-helper: matched plane=%u to crtc=%u (fb=%u, %s)\n",
                        plane->plane_id, target_crtc, fb_id, is_primary ? "PRIMARY" : "overlay");
                if (is_primary) {
                    drmModeFreePlane(plane);
                    break;
                }
            }
        }
        drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(planes);

    if (fb_id == 0) {
        return send_error(sock, "no active framebuffer found");
    }

    /* Use persistent DRM fd for ALL operations.
     * On virtio-gpu 3D (virgl), fresh fds don't maintain the GPU context
     * needed for transfer_from_host to actually refresh pixel data.
     * The persistent fd keeps the context alive — same as VNC direct path. */

    /* GetFB2 on persistent fd */
    drmModeFB2 *fb2 = drmModeGetFB2(drm_fd, fb_id);
    if (!fb2) {
        return send_error_errno(sock, "drmModeGetFB2 failed");
    }

    if (fb2->handles[0] == 0) {
        drmModeFreeFB2(fb2);
        return send_error(sock, "handles[0]==0 (CAP_SYS_ADMIN not set?)");
    }

    /* Reject absurd geometry before any size computation: bound pitch*height so a
     * hostile framebuffer cannot overflow size_t, and so the uint32 data_size we
     * put on the wire cannot disagree with the size_t payload we send. Cap at one
     * 8K BGRA frame (~126 MB, well under UINT32_MAX). */
    if (fb2->pitches[0] == 0 || fb2->height == 0 ||
        (size_t)fb2->height > ((size_t)7680 * 4320 * 4) / fb2->pitches[0]) {
        /* This early return is BEFORE the gem_handle assignment + out: label, so
         * close the just-minted handle here or the reject path leaks it. */
        helper_gem_close(drm_fd, fb2->handles[0]);
        drmModeFreeFB2(fb2);
        return send_error(sock, "rejecting framebuffer geometry (too large)");
    }

    uint32_t gem_handle = fb2->handles[0];

    /* virtio_gpu: transfer pixels from host GPU to guest RAM, then wait. */
    if (is_virtio) {
#ifdef __linux__
        struct drm_virtgpu_3d_transfer_from_host xfer = {0};
        xfer.bo_handle = gem_handle;
        xfer.box.w = fb2->width;
        xfer.box.h = fb2->height;
        xfer.box.d = 1;
        if (drmIoctl(drm_fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &xfer) == 0) {
            struct drm_virtgpu_3d_wait wait_args = {0};
            wait_args.handle = gem_handle;
            drmIoctl(drm_fd, DRM_IOCTL_VIRTGPU_WAIT, &wait_args);
        }
#endif
    }

    /* Decide once how to serve this virtio-gpu scanout. Two sub-cases:
     *   - plain (no virgl 3D): the scanout is a 2D dumb buffer in guest RAM, so
     *     we export it as a DMA-BUF and let the parent map it directly
     *     (zero-copy), instead of copying the whole framebuffer every frame.
     *   - virgl (3D features present): the scanout can be a host-side 3D
     *     resource that a guest CPU mmap reads back black. We still export the
     *     DMA-BUF, but tag it FLAG_VIRGL so the parent reads it back on the GPU
     *     (EGL import + glReadPixels), which sees the real host-rendered pixels.
     * If detection is unavailable, virtio_virgl stays 0; that just means "treat
     * like plain" (export + direct map), which is correct for 2D virtio. */
    static int virtio_detected = 0;  /* 1 once we've probed for 3D features */
    static int virtio_virgl = 0;     /* 1 = confirmed virgl (3D features present) */
    if (is_virtio && !virtio_detected) {
        virtio_detected = 1;
#if defined(__linux__) && defined(DRM_IOCTL_VIRTGPU_GETPARAM)
        uint64_t features = 0;
        struct drm_virtgpu_getparam gp = {
            .param = VIRTGPU_PARAM_3D_FEATURES,
            .value = (uint64_t)(uintptr_t)&features,
        };
        if (drmIoctl(drm_fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp) == 0 && features != 0) {
            virtio_virgl = 1;
        }
        fprintf(stderr, "drmtap-helper: virtio 3D features=%llu -> %s path\n",
                (unsigned long long)features,
                virtio_virgl ? "DMA-BUF + GPU readback (virgl)"
                             : "zero-copy DMA-BUF");
#endif
    }

    /* Build metadata from the framebuffer (independent of how we move pixels). */
    mapped_size = (size_t)fb2->pitches[0] * fb2->height;
    struct grab_metadata meta = {0};
    meta.width = fb2->width;
    meta.height = fb2->height;
    meta.stride = fb2->pitches[0];
    meta.format = fb2->pixel_format;
    meta.fb_id = fb_id;
    /* fb2->modifier is valid only when the FB carries the DRM_MODE_FB_MODIFIERS flag;
     * otherwise it is undefined. Report DRM_FORMAT_MOD_INVALID when the flag is clear
     * so the parent's EGL import infers the real layout instead of trusting a bogus
     * 0/LINEAR on a driver that is actually tiling (the XR30 class). INVALID is
     * non-zero, so the export-vs-dumb-map branch below correctly routes it to the
     * EGL-import path (with the dumb-map fallback still intact if export fails). */
    meta.modifier = (fb2->flags & DRM_MODE_FB_MODIFIERS)
                        ? fb2->modifier
                        : DRM_FORMAT_MOD_INVALID;

    /* HDR transfer + peak from the connector, so the library can decide whether
     * to tone-map (vs a plain bit-depth reduction for SDR 10-bit). */
    read_hdr_metadata(drm_fd, matched_crtc, &meta.hdr_eotf, &meta.hdr_max_nits);
    if (meta.hdr_eotf == 2 || meta.hdr_eotf == 3) {
        fprintf(stderr, "drmtap-helper: HDR scanout eotf=%u peak=%u nits\n",
                meta.hdr_eotf, meta.hdr_max_nits);
    }

    drmModeFreeFB2(fb2);

    /* Stamp frame with sequence number and timestamp */
    static uint32_t seq_counter = 0;
    seq_counter++;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    meta.seq = seq_counter;
    meta.timestamp_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    int ret;

    /* Export the GEM buffer as a DMA-BUF fd and pass it via SCM_RIGHTS so the
     * parent maps it directly — no per-frame pixel copy over the socket. Two
     * cases take this path:
     *   - tiled (non-linear) non-virtio framebuffers: the parent EGL-imports and
     *     GPU-deswizzles. Done BEFORE any MODE_MAP_DUMB on purpose: scanout
     *     buffers on some drivers are not dumb-mappable (nvidia-drm on Tegra
     *     fails MODE_MAP_DUMB with "Failed to lookup gem object"), so requiring a
     *     dumb map first would break capture there even though the export works.
     *   - virtio-gpu (plain or virgl): a linear scanout the parent either maps
     *     directly (plain, guest RAM) or reads back on the GPU (virgl, tagged
     *     FLAG_VIRGL — a CPU mmap of a host-side resource is black). Replaces the
     *     V2 pixel copy, dropping the helper's per-frame CPU cost.
     * Either way, if the export fails we fall through to the dumb-map pixel path. */
    if (meta.modifier != 0 /* DRM_FORMAT_MOD_LINEAR */ || is_virtio) {
        int prime_fd = -1;
        /* Export the scanout DMA-BUF READ-ONLY. Capture only ever reads the
         * framebuffer, and the fd is passed to an unprivileged process over
         * SCM_RIGHTS; DRM_RDWR (== O_RDWR) would let that process create a
         * writable mapping of the active scanout on drivers that honor it,
         * turning a read-only capture into scanout tampering. Dropping DRM_RDWR
         * yields an O_RDONLY descriptor (matches the in-library direct path in
         * drm_grab.c). */
        int prime_ret = drmPrimeHandleToFD(drm_fd, gem_handle,
                                            DRM_CLOEXEC, &prime_fd);
        if (prime_ret == 0 && prime_fd >= 0) {
            meta.flags = FLAG_HAS_DMABUF;
            if (virtio_virgl) {
                meta.flags |= FLAG_VIRGL;  /* parent must GPU-read it back */
            }
            meta.data_size = 0;  /* no pixel data follows */
            ret = send_fd(sock, prime_fd, &meta, sizeof(meta));
            close(prime_fd);
            goto out;
        }
        /* Export failed. For a confirmed virgl scanout the dumb-map fallback
         * below would read back black (host-side resource), so fail closed
         * rather than send a bogus all-black frame. */
        if (virtio_virgl) {
            ret = send_error(sock, "virgl DMA-BUF export failed "
                                   "(no usable CPU readback for a host scanout)");
            goto out;
        }
        /* Otherwise fall through to the dumb-map pixel path. This is safe even for an
         * INVALID (unknown-layout) modifier: a driver that TILES the scanout also
         * implements PRIME export, so the export above would have succeeded and we
         * would not be here. A buffer that reaches this fallback (export failed) is in
         * practice a LINEAR scanout on a non-PRIME driver (plain virtio-gpu guest RAM,
         * simpledrm, legacy AddFB), whose dumb-mapped pixels the parent reconstructs
         * correctly from stride/format/dims. */
        fprintf(stderr,
            "drmtap-helper: drmPrimeHandleToFD failed (%d), falling back to pixels\n",
            prime_ret);
    }

    /* Pixel-data path (linear modifier, virtio, or DMA-BUF export failed):
     * dumb-map the framebuffer in this process and send the pixels directly. */
    struct drm_mode_map_dumb map = {0};
    map.handle = gem_handle;
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        ret = send_error_errno(sock, "MODE_MAP_DUMB failed");
        goto out;
    }
    mapped = mmap(NULL, mapped_size, PROT_READ, MAP_SHARED,
                  drm_fd, map.offset);
    if (mapped == MAP_FAILED) {
        ret = send_error_errno(sock, "mmap failed");
        goto out;
    }
    meta.flags = 0;
    meta.data_size = (uint32_t)mapped_size;

    /* Debug: print pixel values to verify freshness */
    {
        static int frame_num = 0;
        frame_num++;
        if (frame_num <= 5 || frame_num % 60 == 0) {
            uint32_t *pixels = (uint32_t *)mapped;
            uint32_t stride_px = meta.stride / 4;
            uint32_t p_top = pixels[960 < (meta.stride/4) ? 960 : 0];
            uint32_t p_clock = (94 * stride_px + 960 < mapped_size/4) ?
                               pixels[94 * stride_px + 960] : 0;
            uint32_t p_mid = (500 * stride_px + 960 < mapped_size/4) ?
                             pixels[500 * stride_px + 960] : 0;
            fprintf(stderr,
                "drmtap-helper: frame=%d fb=%u mod=0x%lx top=%08x clock=%08x mid=%08x\n",
                frame_num, meta.fb_id, (unsigned long)meta.modifier,
                p_top, p_clock, p_mid);
        }
    }

    ret = send_all(sock, &meta, sizeof(meta));
    if (ret == 0) {
        ret = send_all(sock, mapped, mapped_size);
    }

    /* Cleanup — munmap but DON'T close drm_fd (it's persistent). */
    munmap(mapped, mapped_size);

out:
    helper_gem_close(drm_fd, gem_handle);
    return ret;
}

/* ========================================================================= */
/* Main event loop                                                           */
/* ========================================================================= */

/* Restrict the device this CAP_SYS_ADMIN process will open to the /dev/dri
 * directory. Canonicalize with realpath() so symlinks/.. cannot escape the
 * allowlist, and reject anything that does not resolve under /dev/dri/.
 * Returns 1 if allowed and writes the canonical path into `resolved`
 * (PATH_MAX bytes). */
static int device_path_allowed(const char *path, char *resolved) {
    if (!realpath(path, resolved)) {
        return 0;
    }
    return strncmp(resolved, "/dev/dri/", 9) == 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* Validate socket fd. F_GETFD only tells us fd 3 is open; SO_PEERCRED
     * additionally requires it to be a connected AF_UNIX socket and tells us who
     * is on the other end. This is defense-in-depth, not access control: it
     * rejects being exec'd with fd 3 pointing at an arbitrary open file, or by a
     * different user. Real access control to DRM scanout is enforced by the
     * helper's install permissions (root:rustdesk-capture 0750), not here. */
    if (fcntl(HELPER_SOCKET_FD, F_GETFD) < 0) {
        fprintf(stderr,
                "drmtap-helper: fd %d not valid — must be spawned by "
                "libdrmtap, not run directly\n", HELPER_SOCKET_FD);
        return 1;
    }
    {
        struct ucred peer;
        socklen_t plen = sizeof(peer);
        if (getsockopt(HELPER_SOCKET_FD, SOL_SOCKET, SO_PEERCRED,
                       &peer, &plen) != 0) {
            fprintf(stderr,
                    "drmtap-helper: fd %d is not a socket — must be spawned by "
                    "libdrmtap, not run directly\n", HELPER_SOCKET_FD);
            return 1;
        }
        if (peer.uid != getuid()) {
            fprintf(stderr,
                    "drmtap-helper: refusing to serve peer uid %u (expected %u)\n",
                    (unsigned)peer.uid, (unsigned)getuid());
            return 1;
        }
    }

    int sock = HELPER_SOCKET_FD;

    /* Default device path — can be overridden via argv */
    const char *device = "/dev/dri/card0";
    if (argc > 1) {
        device = argv[1];
    }

    /* The device path comes solely from argv[1] -- the value the library selected
     * and passed. A CAP_SYS_ADMIN process must not take device selection from the
     * environment (the library itself ignores DRM_DEVICE when privileged, since
     * 0.4.11), so DRM_DEVICE is deliberately NOT honored here. */

    /* The device path is attacker-influenceable (argv / DRM_DEVICE) and we run
     * with CAP_SYS_ADMIN, so refuse anything that does not canonicalize under
     * /dev/dri/ before we open it. */
    char resolved_device[PATH_MAX];
    if (!device_path_allowed(device, resolved_device)) {
        fprintf(stderr,
                "drmtap-helper: refusing device path '%s' "
                "(must resolve under /dev/dri/)\n", device);
        return 1;
    }
    device = resolved_device;

    /* No new privileges (must be set before seccomp; also blocks gaining privs
     * via any exec). */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        perror("drmtap-helper: prctl(PR_SET_NO_NEW_PRIVS)");
        return 1;
    }

    /* ================================================================
     * Persistent DRM fd — opened ONCE, reused for all frames.
     * This is the key performance optimization: avoids the expensive
     * open()/close() + drmSetClientCap() on every frame.
     *
     * Opened BEFORE seccomp so the filter can forbid open/openat entirely: the
     * grab loop never opens another path, it only reuses this fd. Opened
     * read-only because we issue only read ioctls (GetFB2 / PrimeHandleToFD /
     * SetClientCap) and never modify KMS state; on the DRM master / CAP_SYS_ADMIN
     * path these all work on an O_RDONLY fd.
     * ================================================================ */
    int drm_fd = open(device, O_RDONLY | O_CLOEXEC);
    if (drm_fd < 0) {
        fprintf(stderr, "drmtap-helper: failed to open %s: %s\n",
                device, strerror(errno));
        return 1;
    }
    /* Defensively drop DRM master. This helper only reads scanout (GetFB2 /
     * PrimeHandleToFD) and never modesets, but if it opened the card node while no
     * client held master the kernel made it master implicitly, which would then block
     * a compositor from (re)acquiring master on a VT switch -> a black/frozen display.
     * drmDropMaster returns 0 only when we actually held master; otherwise no-op. */
    if (drmDropMaster(drm_fd) == 0) {
        fprintf(stderr, "drmtap-helper: dropped implicit DRM master on %s\n", device);
    }
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    /* Detect virtio_gpu once */
    int is_virtio = 0;
    drmVersion *ver = drmGetVersion(drm_fd);
    if (ver) {
        if (ver->name_len >= 9 &&
            strncmp(ver->name, "virtio_gpu", ver->name_len) == 0) {
            is_virtio = 1;
        }
        drmFreeVersion(ver);
    }
    fprintf(stderr, "drmtap-helper: persistent fd=%d device=%s virtio=%d\n",
            drm_fd, device, is_virtio);

    /* The whole point of this helper is to run privileged but confined. With the
     * device already open, drop everything but CAP_SYS_ADMIN and install the
     * seccomp filter. If we cannot establish the confinement, refuse to serve
     * rather than run with CAP_SYS_ADMIN unconfined. */
#ifdef HAVE_LIBCAP
    if (drop_caps() != 0) {
        fprintf(stderr,
                "drmtap-helper: refusing to run: could not drop capabilities\n");
        return 1;
    }
#endif

#ifdef HAVE_SECCOMP
    if (install_seccomp() != 0) {
        fprintf(stderr,
                "drmtap-helper: refusing to run: could not install seccomp filter\n");
        return 1;
    }
#endif

    /* Event loop: receive commands, process with persistent fd */
    while (1) {
        helper_cmd_grab_t hcmd;
        /* Read the whole fixed-size command frame. A clean disconnect (EOF at a
         * boundary) or a truncated/partial frame both end the loop; we never act
         * on a short read with an uninitialized crtc_id. */
        if (recv_all(sock, &hcmd, sizeof(hcmd)) != 0) {
            break;
        }
        /* Reject a frame from a mismatched protocol (a stale helper or library
         * binary) or an unknown command type: wire_cmd_valid gates the
         * magic/version/length AND the type at one point, before dispatch. Close
         * the connection WITHOUT a response: the framing is untrustworthy and we
         * cannot know which reply schema the peer expects (grab vs cursor), so
         * any error payload could be misparsed as valid data. A closed socket is
         * an unambiguous failure every client path already handles. Fail closed
         * rather than misparse a frame while holding CAP_SYS_ADMIN. */
        if (!wire_cmd_valid(&hcmd)) {
            break;
        }

        switch (hcmd.type) {
            case CMD_GRAB:
                grab_and_send(sock, drm_fd, hcmd.crtc_id, is_virtio);
                break;

            case CMD_GET_CURSOR:
                cursor_and_send(sock, drm_fd, hcmd.crtc_id);
                break;

            case CMD_QUIT:
                goto done;

            default:
                /* Unreachable: wire_cmd_valid already restricted the type to the
                 * three commands above. Close defensively rather than reply with
                 * a schema the peer may not expect. */
                goto done;
        }
    }

done:
    close(sock);
    return 0;
}
