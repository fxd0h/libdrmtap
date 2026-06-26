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
#include <sys/prctl.h>
#include <limits.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

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

/* Protocol commands */
#define CMD_GRAB 0x01
#define CMD_GET_CURSOR 0x02
#define CMD_QUIT 0xFF

/* Command structure for CMD_GRAB (client to helper) */
struct helper_cmd_grab {
    uint8_t  cmd;           /* CMD_GRAB (0x01) */
    /* cppcheck-suppress unusedStructMember ; explicit wire-format padding */
    uint8_t  _pad1[3];      /* align to 4 bytes */
    uint32_t crtc_id;       /* target CRTC id (0 = auto-select first active) */
};

/* Response status */
#define RESP_OK    0x00
#define RESP_ERROR 0x01

/* Metadata sent before pixel data — must match helper_grab_result_t in library */
#define FLAG_HAS_DMABUF 0x01

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
    /* cppcheck-suppress unusedStructMember ; explicit wire-format padding */
    uint32_t _pad;
};

/* Send a file descriptor via SCM_RIGHTS */
static int send_fd(int sock, int fd, const void *meta, size_t meta_len) {
    struct msghdr msg = {0};
    struct iovec iov;
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } cmsg_buf;

    iov.iov_base = (void *)meta;
    iov.iov_len = meta_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    msg.msg_control = cmsg_buf.buf;
    msg.msg_controllen = sizeof(cmsg_buf.buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    ssize_t n = sendmsg(sock, &msg, MSG_NOSIGNAL);
    if (n < 0) {
        perror("sendmsg SCM_RIGHTS");
        return -1;
    }
    return 0;
}

/* ========================================================================= */
/* Security hardening                                                        */
/* ========================================================================= */

#ifdef HAVE_LIBCAP
// Drop all capabilities except CAP_SYS_ADMIN
static int drop_caps(void) {
    cap_t caps = cap_init();
    if (!caps) {
        perror("cap_set_proc failed"); return -1;
    }

    cap_value_t keep[] = { CAP_SYS_ADMIN };
    if (cap_set_flag(caps, CAP_PERMITTED, 1, keep, CAP_SET) != 0 ||
        cap_set_flag(caps, CAP_EFFECTIVE, 1, keep, CAP_SET) != 0) {
        cap_free(caps);
        perror("cap_set_proc failed"); return -1;
    }

    int ret = cap_set_proc(caps);
    cap_free(caps);

    if (ret == 0) {
        fprintf(stderr, "drmtap-helper: dropped caps, keeping CAP_SYS_ADMIN\n");
    }
    return ret;
}
#endif

#ifdef HAVE_SECCOMP
// Install seccomp filter allowing only needed syscalls
static int install_seccomp(void) {
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_KILL_PROCESS);
    if (!ctx) {
        perror("cap_set_proc failed"); return -1;
    }

    /* NOTE: open/openat are deliberately NOT allowed. The DRM device is opened
     * once in main() before this filter is installed, and the grab loop only
     * ever reuses that fd — so a compromised helper cannot open arbitrary files
     * even though it holds CAP_SYS_ADMIN. */
    int allowed[] = {
        SCMP_SYS(read), SCMP_SYS(write), SCMP_SYS(close),
        SCMP_SYS(ioctl),       /* DRM ioctls */
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
        if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, allowed[i], 0) != 0) {
            seccomp_release(ctx);
            perror("cap_set_proc failed"); return -1;
        }
    }

    int ret = seccomp_load(ctx);
    seccomp_release(ctx);

    if (ret == 0) {
        fprintf(stderr, "drmtap-helper: seccomp filter installed\n");
    }
    return ret;
}
#endif

/* ========================================================================= */
/* Socket helpers                                                            */
/* ========================================================================= */

// Send all bytes, handling partial writes
static int send_all(int sock, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            perror("cap_set_proc failed"); return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

// Send error: metadata with data_size=0
static int send_error(int sock, const char *reason) {
    fprintf(stderr, "drmtap-helper: %s\n", reason);
    struct grab_metadata meta = {0};  /* data_size=0 signals error */
    send_all(sock, &meta, sizeof(meta));
    perror("cap_set_proc failed"); return -1;
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

    int prime_fd = -1;
    void *mapped = MAP_FAILED;
    size_t size = (size_t)fb2->width * fb2->height * 4;
    if (drmPrimeHandleToFD(drm_fd, fb2->handles[0], O_RDONLY | O_CLOEXEC,
                           &prime_fd) == 0 && prime_fd >= 0) {
        mapped = mmap(NULL, size, PROT_READ, MAP_SHARED, prime_fd, 0);
    }
    drmModeFreeFB2(fb2);

    if (mapped == MAP_FAILED) {
        if (prime_fd >= 0) close(prime_fd);
        send_all(sock, &meta, sizeof(meta));  /* visible=0 (couldn't read) */
        return 0;
    }

    meta.visible = 1;
    meta.data_size = (uint32_t)size;
    send_all(sock, &meta, sizeof(meta));
    send_all(sock, mapped, size);
    munmap(mapped, size);
    close(prime_fd);
    return 0;
}

/* ========================================================================= */
/* DRM capture (privileged) — reads pixels, sends via socket                 */
/* ========================================================================= */

// Grab current framebuffer. Helper reads pixels in its own process
// (where dumb_mmap returns fresh data) and sends them via the socket.
static int grab_and_send(int sock, int drm_fd, uint32_t target_crtc, int is_virtio) {
    void *mapped = MAP_FAILED;
    size_t mapped_size = 0;

    /* Refresh plane state on persistent fd */
    drmModePlaneRes *planes = drmModeGetPlaneResources(drm_fd);
    if (!planes) {
        return send_error(sock, "drmModeGetPlaneResources failed");
    }

    /* Search for the plane currently bound to the target CRTC */
    uint32_t fb_id = 0;
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
            /* Check plane type — prefer PRIMARY */
            drmModeObjectProperties *props = drmModeObjectGetProperties(
                drm_fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
            int is_primary = 0;
            if (props) {
                for (uint32_t p = 0; p < props->count_props; p++) {
                    drmModePropertyRes *prop = drmModeGetProperty(
                        drm_fd, props->props[p]);
                    if (prop) {
                        if (strcmp(prop->name, "type") == 0 &&
                            props->prop_values[p] == DRM_PLANE_TYPE_PRIMARY) {
                            is_primary = 1;
                        }
                        drmModeFreeProperty(prop);
                    }
                }
                drmModeFreeObjectProperties(props);
            }

            if (is_primary || fb_id == 0) {
                fb_id = plane->fb_id;
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
        return send_error(sock, "drmModeGetFB2 failed");
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

    /* Decide once whether this virtio-gpu is "plain" (no virgl 3D). Without 3D
     * the scanout is a 2D dumb buffer in guest RAM that we can export as a
     * DMA-BUF and let the parent map directly (zero-copy), instead of copying
     * the whole framebuffer over the socket every frame. With virgl the scanout
     * can be a host-side 3D resource, so we keep the proven copy-after-transfer
     * path there until the zero-copy path is validated on virgl. */
    static int virtio_plain = -1;
    if (is_virtio && virtio_plain < 0) {
        virtio_plain = 0;
#if defined(__linux__) && defined(DRM_IOCTL_VIRTGPU_GETPARAM)
        uint64_t features = 0;
        struct drm_virtgpu_getparam gp = {
            .param = VIRTGPU_PARAM_3D_FEATURES,
            .value = (uint64_t)(uintptr_t)&features,
        };
        if (drmIoctl(drm_fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp) == 0 && features == 0) {
            virtio_plain = 1;
        }
        fprintf(stderr, "drmtap-helper: virtio 3D features=%llu -> %s path\n",
                (unsigned long long)features,
                virtio_plain ? "zero-copy DMA-BUF" : "pixel-copy");
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
    meta.modifier = fb2->modifier;

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
     *   - plain virtio-gpu (guest-rendered, no host transfer): a linear scanout
     *     in guest RAM the parent maps directly (no detile). Replaces the V2
     *     pixel copy, dropping the helper's per-frame CPU cost.
     * Either way, if the export fails we fall through to the dumb-map pixel path. */
    if ((meta.modifier != 0 /* DRM_FORMAT_MOD_LINEAR */ && !is_virtio)
        || (is_virtio && virtio_plain)) {
        int prime_fd = -1;
        int prime_ret = drmPrimeHandleToFD(drm_fd, gem_handle,
                                            DRM_CLOEXEC | DRM_RDWR, &prime_fd);
        if (prime_ret == 0 && prime_fd >= 0) {
            meta.flags = FLAG_HAS_DMABUF;
            meta.data_size = 0;  /* no pixel data follows */
            ret = send_fd(sock, prime_fd, &meta, sizeof(meta));
            close(prime_fd);
            return ret;
        }
        /* Export failed — fall through to the dumb-map pixel path. */
        fprintf(stderr,
            "drmtap-helper: drmPrimeHandleToFD failed (%d), falling back to pixels\n",
            prime_ret);
    }

    /* Pixel-data path (linear modifier, virtio, or DMA-BUF export failed):
     * dumb-map the framebuffer in this process and send the pixels directly. */
    struct drm_mode_map_dumb map = {0};
    map.handle = gem_handle;
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        return send_error(sock, "MODE_MAP_DUMB failed");
    }
    mapped = mmap(NULL, mapped_size, PROT_READ, MAP_SHARED,
                  drm_fd, map.offset);
    if (mapped == MAP_FAILED) {
        return send_error(sock, "mmap failed");
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

    /* Also check DRM_DEVICE env var */
    const char *env_dev = getenv("DRM_DEVICE");
    if (env_dev) {
        device = env_dev;
    }

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
        struct helper_cmd_grab hcmd;
        ssize_t n = recv(sock, &hcmd, sizeof(hcmd), 0);

        if (n <= 0) {
            break;
        }

        uint8_t cmd = hcmd.cmd;
        if (n < (ssize_t)sizeof(hcmd)) {
            /* Fallback or partial read — can happen if client still uses old 1-byte protocol
             * but for now we expect the full struct. */
        }

        switch (cmd) {
            case CMD_GRAB:
                grab_and_send(sock, drm_fd, hcmd.crtc_id, is_virtio);
                break;

            case CMD_GET_CURSOR:
                cursor_and_send(sock, drm_fd, hcmd.crtc_id);
                break;

            case CMD_QUIT:
                goto done;

            default:
                send_error(sock, "unknown command");
                break;
        }
    }

done:
    close(sock);
    return 0;
}
