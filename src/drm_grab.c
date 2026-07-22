/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file drm_grab.c
 * @brief Framebuffer capture via DRM/KMS: GetFB2 → PrimeHandleToFD → mmap
 *
 * Capture pipeline:
 *   1. Find primary plane for the target CRTC
 *   2. Refresh fb_id via drmModeGetPlane() (NEVER cache!)
 *   3. Call drmModeGetFB2(fb_id) → get format, handles, strides
 *   4. Check handles[0] == 0 → CAP_SYS_ADMIN missing → need helper
 *   5. drmPrimeHandleToFD() → DMA-BUF fd
 *   6. Optional: mmap for mapped path
 *   7. DMA_BUF_IOCTL_SYNC for read safety
 *
 * virtio_gpu special path:
 *   Parallels/QEMU VMs with virtio 3D render framebuffers on the host GPU.
 *   DMA-BUF mmap returns zeros. We detect this driver and use:
 *     a. DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST to pull pixels from host
 *     b. DRM_IOCTL_VIRTGPU_WAIT for transfer completion
 *     c. DRM_IOCTL_MODE_MAP_DUMB to mmap via DRM fd (not prime fd)
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/dma-buf.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

/* virtio_gpu header for TRANSFER_FROM_HOST ioctl */
#ifdef __has_include
#  if __has_include(<virtgpu_drm.h>)
#    include <virtgpu_drm.h>
#    define HAVE_VIRTGPU 1
#  endif
#endif
#ifndef HAVE_VIRTGPU
#  define HAVE_VIRTGPU 0
#endif

#include "drmtap_internal.h"
#include <drm_fourcc.h>

/* Forward declaration */
static int gpu_auto_process(drmtap_ctx *ctx, void *data,
                            drmtap_frame_info *frame, int force_egl);

/* Close a GEM handle returned by drmModeGetFB2 on the ctx's DRM fd. Each
 * drmModeGetFB2 mints a fresh handle the caller must close (it is a separate
 * kernel BO reference from the prime fd); not closing it leaks a handle on
 * every grab. No-op for handle 0 (helper path) or a NULL ctx. */
static void drmtap_gem_close(drmtap_ctx *ctx, uint32_t handle) {
    if (!ctx || handle == 0) {
        return;
    }
    struct drm_gem_close gc;
    memset(&gc, 0, sizeof(gc));
    gc.handle = handle;
    drmIoctl(ctx->drm_fd, DRM_IOCTL_GEM_CLOSE, &gc);
}

/* Test hook: DRMTAP_FORCE_MMAP_FAIL=1 makes the fast-path cache-miss drop a
 * successful CPU mapping so the EGL-detile fd fallback runs on any EGL-capable GPU,
 * not only a discrete/tiled one that genuinely refuses the mmap. Off by default. */
static int drmtap_force_mmap_fail(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DRMTAP_FORCE_MMAP_FAIL");
        v = (e && e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* ========================================================================= */
/* Internal state for a captured frame (stored in frame->_priv)              */
/* ========================================================================= */

typedef struct {
    int prime_fd;           /* DMA-BUF fd from PrimeHandleToFD */
    int helper_drm_fd;      /* DRM fd from helper (needs close on release) */
    void *mapped;           /* mmap'd or malloc'd pixel buffer */
    size_t mapped_size;     /* size of mapped region */
    uint32_t gem_handle;    /* GEM handle (needs close) */
    int used_dumb_map;      /* 1 if mapped via dumb buffer (virtio_gpu) */
    int is_heap_buf;        /* 1 if mapped is malloc'd (helper v2 pixel path) */
    int sync_started;       /* 1 if dmabuf_sync_start succeeded — release issues
                               the matching SYNC_END only then (no END w/o START) */
} frame_priv_t;

/* ========================================================================= */
/* Plane discovery                                                           */
/* ========================================================================= */

/* Read the HDR transfer + peak luminance from the connector driving `crtc_id`
 * into ctx->cur_hdr_eotf / cur_hdr_max_nits, for the direct (no-helper) capture
 * path. Mirrors read_hdr_metadata in the helper. Best-effort: any failure leaves
 * it SDR (0), so a non-HDR display just means no tone-mapping. */
/* Resolve the CRTC bound to a connector via the atomic CRTC_ID property; the
 * legacy encoder link (drmModeGetEncoder(conn->encoder_id)->crtc_id) reads 0 for
 * an atomically-bound connector under atomic KMS (Wayland), so an HDR display never
 * matched its CRTC and never got tone-mapped. Returns 0 if unbound/unavailable. */
static uint32_t connector_crtc_atomic(int drm_fd, uint32_t connector_id) {
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

static void read_hdr_metadata_direct(drmtap_ctx *ctx, uint32_t crtc_id) {
    ctx->cur_hdr_eotf = 0;
    ctx->cur_hdr_max_nits = 0;
    if (crtc_id == 0) {
        return;
    }
    drmModeRes *res = drmModeGetResources(ctx->drm_fd);
    if (!res) {
        return;
    }
    /* Match on the atomic CRTC_ID property (see connector_crtc_atomic), not the
     * legacy encoder link. GetConnectorCurrent reads cached kernel state instead of
     * forcing a hardware connector probe on every do_grab. */
    uint32_t conn_id = 0;
    for (int i = 0; i < res->count_connectors && conn_id == 0; i++) {
        drmModeConnector *conn =
            drmModeGetConnectorCurrent(ctx->drm_fd, res->connectors[i]);
        if (!conn) {
            continue;
        }
        if (conn->connection == DRM_MODE_CONNECTED &&
            connector_crtc_atomic(ctx->drm_fd, conn->connector_id) == crtc_id) {
            conn_id = conn->connector_id;
        }
        drmModeFreeConnector(conn);
    }
    drmModeFreeResources(res);
    if (conn_id == 0) {
        return;
    }
    drmModeObjectProperties *props = drmModeObjectGetProperties(
        ctx->drm_fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
    if (!props) {
        return;
    }
    for (uint32_t p = 0; p < props->count_props; p++) {
        drmModePropertyRes *prop = drmModeGetProperty(ctx->drm_fd, props->props[p]);
        if (!prop) {
            continue;
        }
        if (strcmp(prop->name, "HDR_OUTPUT_METADATA") == 0 &&
            props->prop_values[p] != 0) {
            drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(
                ctx->drm_fd, (uint32_t)props->prop_values[p]);
#if HAVE_HDR_METADATA
            if (blob && blob->data &&
                blob->length >= sizeof(struct hdr_output_metadata)) {
                const struct hdr_output_metadata *m = blob->data;
                const struct hdr_metadata_infoframe *inf =
                    &m->hdmi_metadata_type1;
                ctx->cur_hdr_eotf = inf->eotf;
                ctx->cur_hdr_max_nits = inf->max_cll
                    ? inf->max_cll : inf->max_display_mastering_luminance;
            }
#endif
            if (blob) {
                drmModeFreePropertyBlob(blob);
            }
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    if (ctx->cur_hdr_eotf == DRMTAP_EOTF_PQ) {
        drmtap_debug_log(ctx, "direct: HDR scanout eotf=%u peak=%u nits",
                         ctx->cur_hdr_eotf, ctx->cur_hdr_max_nits);
    }
}

// Find the primary plane attached to the target CRTC
// Returns the plane_id or 0 on failure
static uint32_t find_primary_plane(drmtap_ctx *ctx) {
    drmModePlaneRes *planes = drmModeGetPlaneResources(ctx->drm_fd);
    if (!planes) {
        drmtap_debug_log(ctx, "drmModeGetPlaneResources failed: %s",
                         strerror(errno));
        return 0;
    }

    uint32_t target_crtc = ctx->crtc_id;
    uint32_t result = 0;

    /* If no CRTC selected, pick the first active one */
    if (target_crtc == 0) {
        drmModeRes *res = drmModeGetResources(ctx->drm_fd);
        if (res) {
            for (int i = 0; i < res->count_crtcs; i++) {
                drmModeCrtc *crtc = drmModeGetCrtc(ctx->drm_fd, res->crtcs[i]);
                if (crtc) {
                    if (crtc->mode_valid) {
                        target_crtc = crtc->crtc_id;
                        ctx->crtc_id = target_crtc;
                        drmtap_debug_log(ctx, "auto-selected CRTC %u", target_crtc);
                        drmModeFreeCrtc(crtc);
                        break;
                    }
                    drmModeFreeCrtc(crtc);
                }
            }
            drmModeFreeResources(res);
        }
    }

    if (target_crtc == 0) {
        drmtap_debug_log(ctx, "no active CRTC found");
        drmModeFreePlaneResources(planes);
        return 0;
    }

    /* Search for the plane currently bound to the target CRTC */
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *plane = drmModeGetPlane(ctx->drm_fd, planes->planes[i]);
        if (!plane) {
            continue;
        }

        /* Skip planes not bound to our target CRTC */
        if (plane->crtc_id != target_crtc) {
            drmModeFreePlane(plane);
            continue;
        }

        int is_primary = 0;

        /* Check if it has an active framebuffer (it should if bound, but let's be safe) */
        if (plane->fb_id != 0) {
            /* Check plane type to prefer PRIMARY */
            drmModeObjectProperties *props = drmModeObjectGetProperties(
                ctx->drm_fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);

            if (props) {
                for (uint32_t p = 0; p < props->count_props; p++) {
                    drmModePropertyRes *prop = drmModeGetProperty(
                        ctx->drm_fd, props->props[p]);
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

            if (is_primary || result == 0) {
                result = plane->plane_id;
                drmtap_debug_log(ctx, "find_primary_plane: matched plane=%u to crtc=%u (fb=%u, %s)",
                                 plane->plane_id, target_crtc, plane->fb_id,
                                 is_primary ? "PRIMARY" : "overlay");
            }
        }

        drmModeFreePlane(plane);
        if (result != 0 && is_primary) {
            /* Found the primary plane for this CRTC, we are done */
            break;
        }
    }

    drmModeFreePlaneResources(planes);

    /* Direct (no-helper) path: read the connector HDR metadata for this CRTC so
     * the conversion path can tone-map. In helper mode this comes over the wire
     * instead (drmtap_helper_grab sets ctx->cur_hdr_eotf). */
    read_hdr_metadata_direct(ctx, ctx->crtc_id);

    return result;
}

/* ========================================================================= */
/* DMA-BUF sync helpers                                                      */
/* ========================================================================= */

static int dmabuf_sync_start(int fd) {
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ
    };
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

static int dmabuf_sync_end(int fd) {
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ
    };
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

/* Ensure *buf holds at least `size` bytes, growing once and never shrinking so
 * steady-state capture reuses one allocation instead of malloc/free per frame.
 * Caps the allocation at DRMTAP_MAX_FB_BYTES as a guard against a bogus/hostile
 * framebuffer geometry forcing an unbounded request. Contents are not preserved
 * across a grow. Returns 0, -EINVAL (zero size), -EFBIG (over the cap), or
 * -ENOMEM. Shared across modules (gpu_egl.c reads back into the same ctx
 * buffer), hence not static. */
int drmtap_ensure_buf(void **buf, size_t *cap, size_t size) {
    if (size == 0) {
        return -EINVAL;
    }
    if (size > DRMTAP_MAX_FB_BYTES) {
        return -EFBIG;
    }
    if (*buf && *cap >= size) {
        return 0;
    }
    free(*buf);
    *buf = malloc(size);
    if (!*buf) {
        *cap = 0;
        return -ENOMEM;
    }
    *cap = size;
    return 0;
}

/* ========================================================================= */
/* virtio_gpu transfer helpers                                               */
/* ========================================================================= */

// Check if the driver is virtio_gpu (needs special capture path)
static int is_virtio_gpu(drmtap_ctx *ctx) {
    const char *driver = drmtap_gpu_driver(ctx);
    return (driver && strcmp(driver, "virtio_gpu") == 0);
}

// Pull framebuffer data from host GPU to guest memory
// This is required for virtio_gpu 3D (virgl) where the compositor
// renders on the host and DMA-BUF mmap returns zeros
static int virtio_transfer_from_host(drmtap_ctx *ctx, uint32_t handle,
                                     uint32_t width, uint32_t height) {
#if HAVE_VIRTGPU
    struct drm_virtgpu_3d_transfer_from_host xfer = {0};
    xfer.bo_handle = handle;
    xfer.box.w = width;
    xfer.box.h = height;
    xfer.box.d = 1;
    int ret = drmIoctl(ctx->drm_fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST,
                       &xfer);
    if (ret != 0) {
        drmtap_debug_log(ctx, "TRANSFER_FROM_HOST failed: %s",
                         strerror(errno));
        return -errno;
    }

    /* Wait for transfer completion */
    struct drm_virtgpu_3d_wait wait_args = {0};
    wait_args.handle = handle;
    ret = drmIoctl(ctx->drm_fd, DRM_IOCTL_VIRTGPU_WAIT, &wait_args);
    if (ret != 0) {
        drmtap_debug_log(ctx, "VIRTGPU_WAIT failed: %s", strerror(errno));
        return -errno;
    }

    drmtap_debug_log(ctx, "virtio: transferred %ux%u from host",
                     width, height);
    return 0;
#else
    (void)ctx; (void)handle; (void)width; (void)height;
    return -ENOTSUP;
#endif
}

// Map a GEM handle via the dumb buffer path (DRM fd, not prime fd)
static void *virtio_dumb_mmap(drmtap_ctx *ctx, uint32_t handle, size_t size) {
    struct drm_mode_map_dumb map = {0};
    map.handle = handle;
    if (drmIoctl(ctx->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        drmtap_debug_log(ctx, "MODE_MAP_DUMB failed: %s", strerror(errno));
        return MAP_FAILED;
    }
    return mmap(NULL, size, PROT_READ, MAP_SHARED, ctx->drm_fd, map.offset);
}

/* ========================================================================= */
/* Core capture logic                                                        */
/* ========================================================================= */

/* Reject framebuffer geometry whose byte size (stride * height) would overflow
 * size_t (a concern on 32-bit builds) or exceed DRMTAP_MAX_FB_BYTES, before any
 * downstream multiply feeds an allocation or mmap. stride/height come from
 * drmModeGetFB2 or the helper wire and are not under our control, so validating
 * once at each entry point keeps all later stride*height computations safe. */
static int validate_fb_size(uint32_t stride, uint32_t height) {
    if (stride == 0 || height == 0) {
        return -EINVAL;
    }
    if ((size_t)height > DRMTAP_MAX_FB_BYTES / stride) {
        return -EFBIG;
    }
    return 0;
}

// Internal capture that populates frame_info
// If do_mmap is true, also maps the pixel data to frame->data
static int do_grab(drmtap_ctx *ctx, drmtap_frame_info *frame, int do_mmap) {
    int ret;
    frame_priv_t *priv = NULL;
    drmModeFB2 *fb2 = NULL;
    int prime_fd = -1;

    if (ctx->is_render_only) {
        drmtap_set_error(ctx, "context is render-only (drmtap_open_render); "
                         "grab needs a KMS context from drmtap_open");
        return -ENOTSUP;
    }

    /* Step 1: Find the primary plane */
    uint32_t plane_id = find_primary_plane(ctx);
    if (plane_id == 0) {
        drmtap_set_error(ctx, "No active plane found for capture");
        return -ENODEV;
    }

    /* Step 2: Refresh plane → get CURRENT fb_id (never cache!) */
    drmModePlane *plane = drmModeGetPlane(ctx->drm_fd, plane_id);
    if (!plane || plane->fb_id == 0) {
        drmtap_set_error(ctx, "Plane %u has no framebuffer", plane_id);
        if (plane) {
            drmModeFreePlane(plane);
        }
        return -ENODEV;
    }
    uint32_t fb_id = plane->fb_id;
    drmModeFreePlane(plane);

    /* Step 3: GetFB2 → format, handles, strides, modifier */
    fb2 = drmModeGetFB2(ctx->drm_fd, fb_id);
    if (!fb2) {
        ret = -errno;
        drmtap_set_error(ctx, "drmModeGetFB2(%u) failed: %s",
                         fb_id, strerror(errno));
        return ret;
    }

    drmtap_debug_log(ctx, "FB2: %ux%u fmt=%.4s modifier=0x%lx",
                     fb2->width, fb2->height,
                     (const char *)&fb2->pixel_format,
                     (unsigned long)fb2->modifier);

    ret = validate_fb_size(fb2->pitches[0], fb2->height);
    if (ret != 0) {
        drmtap_set_error(ctx, "rejecting framebuffer geometry %ux%u stride=%u",
                         fb2->width, fb2->height, fb2->pitches[0]);
        drmModeFreeFB2(fb2);
        return ret;
    }

    /* Cache multi-plane info for EGL CCS import */
    ctx->fb2_num_planes = 0;
    for (int p = 0; p < 4; p++) {
        ctx->fb2_pitches[p] = fb2->pitches[p];
        ctx->fb2_offsets[p] = fb2->offsets[p];
        if (fb2->handles[p] || fb2->pitches[p]) {
            ctx->fb2_num_planes = p + 1;
        }
    }

    /* Step 4 & 5: Check handles[0] and attempt export to check CAP_SYS_ADMIN */
    int needs_helper = 0;

    if (fb2->handles[0] == 0) {
        needs_helper = 1;
    } else {
        /* Export DMA-BUF to test if we actually have permission */
        ret = drmPrimeHandleToFD(ctx->drm_fd, fb2->handles[0],
                                  O_RDONLY | O_CLOEXEC, &prime_fd);
        if (ret < 0 && (errno == EACCES || errno == EPERM)) {
            /* Export denied: the capture goes through the helper (which mints its
             * own handle over IPC) and never adopts this one into priv->gem_handle.
             * Close the handle GetFB2 just minted, or the needs_helper path -- and
             * its per-frame V2/V3 success returns -- leaks it. */
            needs_helper = 1;
            drmtap_gem_close(ctx, fb2->handles[0]);
        } else if (ret < 0) {
            ret = -errno;
            drmtap_set_error(ctx, "drmPrimeHandleToFD failed: %s", strerror(errno));
            goto cleanup;
        }
    }

    if (needs_helper) {
        drmtap_debug_log(ctx,
            "No CAP_SYS_ADMIN (needs helper), trying helper...");

        /* Receive buffer for the helper to fill. ctx-owned, grow-once and
         * reused across grabs (never per-frame churn), capped — see
         * drmtap_ensure_buf. Freed in drmtap_close(). */
        size_t buf_size = (size_t)fb2->pitches[0] * fb2->height;
        ret = drmtap_ensure_buf(&ctx->pixel_buf, &ctx->pixel_buf_size, buf_size);
        if (ret != 0) {
            if (ret == -EFBIG) {
                drmtap_set_error(ctx,
                    "framebuffer too large: %zu bytes (max %zu)",
                    buf_size, (size_t)DRMTAP_MAX_FB_BYTES);
            }
            drmModeFreeFB2(fb2);
            return ret;
        }
        void *pixel_buf = ctx->pixel_buf;

        /* Helper reads pixels in its own process and sends via socket */
        helper_grab_result_t hresult;
        ret = drmtap_helper_grab(ctx, &hresult, pixel_buf, buf_size);
        if (ret < 0) {
            drmtap_set_error(ctx,
                "No CAP_SYS_ADMIN and helper failed (ret=%d). Install the "
                "helper, restricting it first so the capability is not "
                "world-usable: sudo cp drmtap-helper /usr/local/bin/ && "
                "sudo chown root:<capture-group> /usr/local/bin/drmtap-helper && "
                "sudo chmod 0750 /usr/local/bin/drmtap-helper && "
                "sudo setcap cap_sys_admin+ep /usr/local/bin/drmtap-helper "
                "(see SECURITY.md)",
                ret);
            drmModeFreeFB2(fb2);
            return -EACCES;
        }

        /* Allocate private state */
        priv = calloc(1, sizeof(frame_priv_t));
        if (!priv) {
            /* The helper may already have handed us a DMA-BUF fd over
             * SCM_RIGHTS; close it on every pre-adoption error return. */
            if (hresult.dmabuf_fd >= 0) {
                close(hresult.dmabuf_fd);
            }
            drmModeFreeFB2(fb2);
            return -ENOMEM;
        }
        priv->helper_drm_fd = -1;
        priv->prime_fd = -1;
        priv->gem_handle = 0;
        /* pixel_buf is ctx-owned and reused; priv must never free/munmap it. */
        priv->mapped = MAP_FAILED;
        priv->mapped_size = 0;
        priv->used_dumb_map = 1;        /* skip dmabuf_sync in release */
        priv->is_heap_buf = 0;          /* ctx owns the buffer; do not free */

        /* Fill frame info from helper metadata */
        memset(frame, 0, sizeof(*frame));
        frame->width = hresult.wire.width;
        frame->height = hresult.wire.height;
        frame->stride = hresult.wire.stride;
        frame->format = hresult.wire.format;
        frame->modifier = hresult.wire.modifier;
        frame->fb_id = hresult.wire.fb_id;
        /* HDR transfer the helper read from the connector — drives tone-mapping. */
        ctx->cur_hdr_eotf = hresult.wire.hdr_eotf;
        ctx->cur_hdr_max_nits = hresult.wire.hdr_max_nits;

        /* The helper validates its own geometry, but it sends stride/height over
         * the wire — re-check before we mmap/size anything from those values. */
        ret = validate_fb_size(frame->stride, frame->height);
        if (ret != 0) {
            drmtap_set_error(ctx, "helper sent invalid geometry %ux%u stride=%u",
                             frame->width, frame->height, frame->stride);
            /* Release the SCM_RIGHTS fd the helper already sent before we
             * reject its geometry, or it leaks toward RLIMIT_NOFILE. */
            if (hresult.dmabuf_fd >= 0) {
                close(hresult.dmabuf_fd);
            }
            free(priv);
            drmModeFreeFB2(fb2);
            return ret;
        }

        /* V3: helper sent DMA-BUF fd via SCM_RIGHTS */
        if (hresult.dmabuf_fd >= 0) {
            /* The helper passed a DMA-BUF fd instead of pixels; ctx->pixel_buf
             * stays allocated for reuse on the next grab. priv is freshly
             * calloc'd (mapped=MAP_FAILED, prime_fd=-1), so there is no prior
             * mapping to release here. */
            int dmabuf_fd = hresult.dmabuf_fd;
            /* virgl scanout: the helper exported the fd but a CPU mmap of it is
             * black (host-side resource), so force the GPU EGL readback path. */
            int is_virgl = (hresult.wire.flags & HELPER_FLAG_VIRGL) != 0;
            int pret = 0;  /* gpu_auto_process result (propagated for virgl) */
            size_t mmap_size = (size_t)frame->stride * frame->height;

            /* mmap the DMA-BUF in the parent */
            void *mapped = mmap(NULL, mmap_size, PROT_READ, MAP_SHARED,
                                dmabuf_fd, 0);

            priv->prime_fd = dmabuf_fd;

            if (mapped != MAP_FAILED) {
                priv->mapped = mapped;
                priv->mapped_size = mmap_size;
                frame->data = mapped;
                frame->dma_buf_fd = dmabuf_fd;
                frame->_priv = priv;

                /* This is a real DMA-BUF the helper exported via PrimeHandleToFD,
                 * not a dumb buffer. Clear the dumb-map flag inherited from the
                 * heap path above and bracket the CPU read with DMA_BUF_SYNC so
                 * cache-coherency is correct (stale pixels otherwise, notably on
                 * ARM/Jetson). The matching SYNC_END runs in the cleanup block. */
                priv->used_dumb_map = 0;
                priv->sync_started = (dmabuf_sync_start(dmabuf_fd) == 0);

                drmtap_debug_log(ctx,
                    "helper V3: mmap'd DMA-BUF fd=%d (%zu bytes), "
                    "EGL deswizzle available",
                    dmabuf_fd, mmap_size);

                if (do_mmap) {
                    pret = gpu_auto_process(ctx, mapped, frame, is_virgl);
                }
            } else {
                /* mmap failed — still have the fd for EGL zero-copy */
                priv->mapped = MAP_FAILED;
                priv->mapped_size = 0;
                frame->data = NULL;
                frame->dma_buf_fd = dmabuf_fd;
                frame->_priv = priv;

                drmtap_debug_log(ctx,
                    "helper V3: mmap failed but DMA-BUF fd=%d available "
                    "for EGL zero-copy", dmabuf_fd);

                if (do_mmap) {
                    pret = gpu_auto_process(ctx, NULL, frame, is_virgl);
                }
            }

            drmModeFreeFB2(fb2);
            /* For a virgl frame the GPU readback is the only way to get real
             * pixels; if it failed, propagate the error rather than handing the
             * caller a black frame. Non-virgl frames keep the prior best-effort
             * behaviour (pret is ignored). */
            if (is_virgl && pret != 0) {
                /* Release everything this frame acquired (the DMA-BUF fd, the
                 * mmap and priv) — a failed grab is not released by the caller,
                 * so returning here without cleanup would leak them each grab. */
                drmtap_frame_release(ctx, frame);
                return pret;
            }
            return 0;
        }

        /* The V2 payload must be exactly one full frame. ctx->pixel_buf is reused
         * across grabs, so accepting a short payload would expose stale pixels
         * from a previous frame behind the advertised stride*height geometry.
         * (frame->stride/height were validated above, so this can't overflow.) */
        if (hresult.wire.data_size != (size_t)frame->stride * frame->height) {
            drmtap_set_error(ctx, "helper V2 payload %u != expected %zu bytes",
                             hresult.wire.data_size,
                             (size_t)frame->stride * frame->height);
            free(priv);
            drmModeFreeFB2(fb2);
            return -EPROTO;
        }

        /* V2 fallback: pixel data received into ctx->pixel_buf. frame->data
         * borrows that buffer — valid until the next grab or drmtap_close();
         * priv must not free it (is_heap_buf=0, mapped=MAP_FAILED above). */
        frame->dma_buf_fd = -1;
        frame->data = pixel_buf;
        frame->_priv = priv;

        if (do_mmap) {
            gpu_auto_process(ctx, pixel_buf, frame, 0);
        }

        drmModeFreeFB2(fb2);
        return 0;
    }

    /* Allocate private state */
    priv = calloc(1, sizeof(frame_priv_t));
    if (!priv) {
        ret = -ENOMEM;
        goto cleanup;
    }
    priv->prime_fd = prime_fd;
    priv->helper_drm_fd = -1;
    priv->gem_handle = fb2->handles[0];
    priv->mapped = MAP_FAILED;
    priv->mapped_size = 0;

    /* Fill frame info */
    memset(frame, 0, sizeof(*frame));
    frame->width = fb2->width;
    frame->height = fb2->height;
    frame->stride = fb2->pitches[0];
    frame->format = fb2->pixel_format;
    /* fb2->modifier is only meaningful when the framebuffer was created with the
     * DRM_MODE_FB_MODIFIERS flag. When the flag is clear the field is undefined
     * (commonly 0, but garbage on some drivers), and trusting a bogus 0/LINEAR on a
     * driver that is actually tiling the scanout corrupts the import (the recurring
     * XR30 / "special fix" class). Report DRM_FORMAT_MOD_INVALID instead so the EGL
     * import omits the modifier attribute and the driver infers the real layout. */
    frame->modifier = (fb2->flags & DRM_MODE_FB_MODIFIERS)
                          ? fb2->modifier
                          : DRM_FORMAT_MOD_INVALID;
    frame->fb_id = fb_id;
    frame->dma_buf_fd = prime_fd;
    frame->data = NULL;
    frame->_priv = priv;

    /* Step 6: mmap if requested (mapped path) */
    if (do_mmap) {
        size_t size = (size_t)fb2->pitches[0] * fb2->height;
        void *mapped = MAP_FAILED;

        /* virtio_gpu special path: transfer from host first, then dumb mmap */
        if (is_virtio_gpu(ctx)) {
            ret = virtio_transfer_from_host(ctx, fb2->handles[0],
                                            fb2->width, fb2->height);
            if (ret < 0) {
                drmtap_debug_log(ctx,
                    "virtio transfer failed, trying standard mmap");
            } else {
                mapped = virtio_dumb_mmap(ctx, fb2->handles[0], size);
                if (mapped != MAP_FAILED) {
                    priv->used_dumb_map = 1;
                    drmtap_debug_log(ctx,
                        "virtio: dumb-mapped %zu bytes at %p", size, mapped);
                }
            }
        }

        /* Standard DMA-BUF mmap path (non-virtio or virtio fallback) */
        if (mapped == MAP_FAILED) {
            dmabuf_sync_start(prime_fd);
            mapped = mmap(NULL, size, PROT_READ, MAP_SHARED,
                          prime_fd, fb2->offsets[0]);
        }

        if (mapped == MAP_FAILED) {
            /* A tiled scanout (notably amdgpu GFX9+) commonly refuses a CPU
             * mmap. The DMA-BUF fd (frame->dma_buf_fd) is still valid, so detile
             * on the GPU via EGL instead of handing back a black frame. Save the
             * mmap errno first: gpu_auto_process below makes syscalls that clobber
             * errno, so it can no longer describe this failure afterwards. */
            int mmap_errno = errno;
            drmtap_debug_log(ctx,
                "mmap failed (%zu bytes: %s), trying EGL detile via DMA-BUF fd",
                size, strerror(mmap_errno));
            frame->data = NULL;
            int rc = gpu_auto_process(ctx, NULL, frame, 0);
            if (!frame->data) {
                /* No pixels: this grab failed. Return an error so the caller can
                 * fall back (e.g. to PipeWire) instead of receiving a black
                 * frame reported as success. Propagate gpu_auto_process's
                 * specific error if it set one (rc != 0 — e.g. its "no CPU
                 * mapping and EGL unavailable" diagnosis), otherwise surface the
                 * original mmap failure. */
                if (rc == 0) {
                    drmtap_set_error(ctx, "mmap failed (%zu bytes): %s",
                                     size, strerror(mmap_errno));
                    rc = mmap_errno ? -mmap_errno : -EIO;
                }
                ret = rc;
                goto cleanup;
            }
        } else {
            /* Invalidate CPU caches with SYNC_START and remember it succeeded so
             * release issues the matching SYNC_END (and only then). Crucial for
             * virtio_gpu where the transfer arrives in system RAM asynchronously. */
            priv->sync_started = (dmabuf_sync_start(prime_fd) == 0);

            priv->mapped = mapped;
            priv->mapped_size = size;
            frame->data = mapped;
            drmtap_debug_log(ctx, "mapped %zu bytes at %p", size, mapped);

            /* Auto-deswizzle tiled framebuffers + format convert */
            gpu_auto_process(ctx, mapped, frame, 0);
        }
    }

    drmModeFreeFB2(fb2);
    return 0;

cleanup:
    if (prime_fd >= 0) {
        close(prime_fd);
    }
    if (fb2) {
        drmModeFreeFB2(fb2);
    }
    /* Close the GEM handle if this error path was reached after the direct grab
     * adopted one (priv->gem_handle set); otherwise it leaks like every grab. */
    if (priv) {
        drmtap_gem_close(ctx, priv->gem_handle);
    }
    free(priv);
    return ret;
}

/* ========================================================================= */
/* Auto-process: deswizzle + format convert based on GPU driver              */
/* ========================================================================= */

/* DRM fourccs for the high-bit-depth scanout formats we reduce to 8-bit. */
#define DRMTAP_FMT_XR30 0x30335258u  /* XRGB2101010 */
#define DRMTAP_FMT_AR30 0x30335241u  /* ARGB2101010 */
#define DRMTAP_FMT_XB30 0x30334258u  /* XBGR2101010 */
#define DRMTAP_FMT_AB30 0x30334241u  /* ABGR2101010 */
#define DRMTAP_FMT_XR48 0x38345258u  /* XRGB16161616 */
#define DRMTAP_FMT_AR48 0x38345241u  /* ARGB16161616 */
#define DRMTAP_FMT_XB48 0x38344258u  /* XBGR16161616 */
#define DRMTAP_FMT_AB48 0x38344241u  /* ABGR16161616 */
#define DRMTAP_FMT_XR24 0x34325258u  /* XRGB8888 */
#define DRMTAP_FMT_XR4H 0x48345258u  /* XRGB16161616F (half-float) */
#define DRMTAP_FMT_AR4H 0x48345241u  /* ARGB16161616F */
#define DRMTAP_FMT_XB4H 0x48344258u  /* XBGR16161616F */
#define DRMTAP_FMT_AB4H 0x48344241u  /* ABGR16161616F */

/* Reduce a LINEAR high-bit-depth scanout (10-bit AR30/XR30 or 16-bit
 * XR48/AR48/XB48/AB48) to 8-bit XRGB8888, tone-mapping when the connector
 * reports HDR (PQ). Output lands in ctx->deswizzle_buf and frame is repointed
 * at it. Returns 1 if it converted, 0 if the format is already 8-bit (caller
 * returns the data as-is), <0 on error. P010 (10-bit YUV overlay video, not the
 * primary desktop scanout) is intentionally not handled — documented limitation. */
static int reduce_linear_to_xrgb8888(drmtap_ctx *ctx, void *data,
                                     drmtap_frame_info *frame) {
    uint32_t fmt = frame->format;
    int is_ar30 = (fmt == DRMTAP_FMT_XR30 || fmt == DRMTAP_FMT_AR30 ||
                   fmt == DRMTAP_FMT_XB30 || fmt == DRMTAP_FMT_AB30);
    int is_rgb16 = (fmt == DRMTAP_FMT_XR48 || fmt == DRMTAP_FMT_AR48 ||
                    fmt == DRMTAP_FMT_XB48 || fmt == DRMTAP_FMT_AB48);
    int is_rgb16f = (fmt == DRMTAP_FMT_XR4H || fmt == DRMTAP_FMT_AR4H ||
                     fmt == DRMTAP_FMT_XB4H || fmt == DRMTAP_FMT_AB4H);
    if (!is_ar30 && !is_rgb16 && !is_rgb16f) {
        return 0;  /* 8-bit RGB (or unknown): leave as-is */
    }

    size_t out_size = (size_t)frame->width * frame->height * 4u;
    int b = drmtap_ensure_buf(&ctx->deswizzle_buf, &ctx->deswizzle_buf_size,
                              out_size);
    if (b != 0) return b;
    uint32_t dst_stride = frame->width * 4u;
    int hdr = (ctx->cur_hdr_eotf == DRMTAP_EOTF_PQ);
    int ret;

    if (is_ar30) {
        if (hdr) {
            ret = drmtap_tonemap_hdr10(data, ctx->deswizzle_buf,
                    frame->width, frame->height, frame->stride, dst_stride,
                    fmt, ctx->cur_hdr_max_nits);
        } else {
            ret = drmtap_convert_format(data, ctx->deswizzle_buf,
                    frame->width, frame->height, frame->stride, dst_stride,
                    fmt, DRMTAP_FMT_XR24);
        }
    } else if (is_rgb16) {
        int bgr = (fmt == DRMTAP_FMT_XB48 || fmt == DRMTAP_FMT_AB48);
        ret = drmtap_convert_rgb16(data, ctx->deswizzle_buf,
                frame->width, frame->height, frame->stride, dst_stride,
                bgr, ctx->cur_hdr_eotf, ctx->cur_hdr_max_nits);
    } else {
        /* Half-float FP16 (XR4H family): linear-light decode + sRGB re-encode. */
        int bgr = (fmt == DRMTAP_FMT_XB4H || fmt == DRMTAP_FMT_AB4H);
        ret = drmtap_convert_rgb16f(data, ctx->deswizzle_buf,
                frame->width, frame->height, frame->stride, dst_stride, bgr);
    }
    if (ret != 0) return ret;

    drmtap_debug_log(ctx, "auto-process: linear %s -> XRGB8888 (%s)",
                     is_ar30 ? "10-bit" : (is_rgb16f ? "FP16" : "16-bit"),
                     hdr ? "HDR tone-mapped" : "SDR");
    frame->data = ctx->deswizzle_buf;
    frame->format = DRMTAP_FMT_XR24;
    frame->stride = dst_stride;
    return 1;
}

static int gpu_auto_process(drmtap_ctx *ctx, void *data,
                            drmtap_frame_info *frame, int force_egl) {
    /* force_egl is set for a virgl scanout: the DMA-BUF holds host-rendered
     * pixels that a CPU mmap reads back black, so we must go down the EGL path
     * (which imports the fd on the GPU) even though there is no usable `data`
     * and the modifier is linear. */
    /* No CPU-mapped pixels. We can still detile on the GPU if the caller passed
     * a DMA-BUF fd: a tiled scanout (notably amdgpu GFX9+) commonly refuses a
     * CPU mmap, so the fd is all we have and the EGL path below imports it
     * directly. Only bail when there is genuinely nothing to work with — no
     * data, no fd, and not a forced virgl readback. */
    if (!data && !force_egl && frame->dma_buf_fd < 0) return 0;

    uint64_t modifier = frame->modifier;
    /* INVALID means the FB advertised no modifier (flag clear), so the layout is
     * unknown. Treat it as linear ONLY when EGL import is unavailable: this is the
     * CPU fallback for the simple drivers that take that path (virtio, embedded),
     * where an unknown scanout is in practice linear. When EGL IS available we must
     * NOT short-circuit to the linear early-return below -- a modern tiling driver
     * leaves the modifier flag clear on a tiled scanout (the XR30 class), and only
     * the EGL path further down, which omits the modifier and lets the driver infer
     * the real layout, decodes it correctly. Tradeoff: a GENUINELY linear flag-clear
     * scanout on an EGL-capable GPU now takes the EGL round-trip too (it is
     * indistinguishable from a secretly-tiled one while the flag is clear), losing the
     * zero-copy path it had before. That is the unavoidable cost of decoding the tiled
     * case correctly; it does not affect a compositor that advertises modifiers (flag
     * set, handled above) nor a no-EGL simple driver (linear here). */
    int is_linear = (modifier == DRM_FORMAT_MOD_LINEAR || modifier == 0 ||
                     (modifier == DRM_FORMAT_MOD_INVALID &&
                      !drmtap_gpu_egl_available(ctx)));

    /* A linear high-bit-depth / HDR scanout still needs reduction to 8-bit
     * (the early-return below is only safe for 8-bit RGB). */
    if (data && is_linear && !force_egl) {
        int red = reduce_linear_to_xrgb8888(ctx, data, frame);
        if (red < 0) return red;
        if (red == 1) return 0;
        /* red == 0: already 8-bit -> fall through to the early-return. */
    }

    /* Linear 8-bit framebuffer: no deswizzle needed (unless a virgl readback is
     * forced — see force_egl above). */
    if (is_linear && !force_egl) {
        return 0;
    }

    /*
     * Non-linear (tiled) framebuffer detected.
     *
     * The mmap'd DMA-BUF is read-only (PROT_READ | MAP_SHARED), so we
     * always deswizzle into a separate buffer (ctx->deswizzle_buf).
     */


    /* Ensure the CPU-deswizzle shadow buffer is allocated (grow-once, capped —
     * see drmtap_ensure_buf). frame->stride/height are validated at every entry point
     * (validate_fb_size), so this multiply cannot overflow. NOTE: this does NOT
     * bound the EGL output, which is always 4-byte RGBA — a sub-4-byte source
     * (e.g. RG16) can expand past stride*height, so the EGL path caps egl_size
     * separately below. */
    size_t size = (size_t)frame->stride * frame->height;
    int bres = drmtap_ensure_buf(&ctx->deswizzle_buf, &ctx->deswizzle_buf_size,
                                 size);
    if (bres != 0) {
        if (bres == -EFBIG) {
            drmtap_set_error(ctx,
                "framebuffer too large for deswizzle: %zu bytes (max %zu)",
                size, (size_t)DRMTAP_MAX_FB_BYTES);
        }
        return bres;
    }

    /* --- EGL path: import DMA-BUF, GPU renders to linear RGBA ---
     *
     * This path requires a valid dma_buf_fd, which is only available when
     * the process has CAP_SYS_ADMIN (direct DRM access). In helper mode,
     * dma_buf_fd == -1 because the V2 protocol sends pixel data via socket
     * instead of passing the fd via SCM_RIGHTS. For CCS-compressed
     * framebuffers, this means we fall to the CPU path which returns
     * -ENOTSUP, and the raw data is returned as-is. */
#ifdef HAVE_EGL
    drmtap_debug_log(ctx, "EGL check: fd=%d avail=%d mod=0x%lx",
            frame->dma_buf_fd, drmtap_gpu_egl_available(ctx),
            (unsigned long)modifier);
    if (frame->dma_buf_fd >= 0 && drmtap_gpu_egl_available(ctx)) {
        void *egl_data = NULL;
        size_t egl_size = 0;
        drmtap_debug_log(ctx, "auto-process: EGL convert (mod=0x%lx)",
                         (unsigned long)modifier);
        int ret = drmtap_gpu_egl_convert(ctx, frame->dma_buf_fd,
                                          frame->width, frame->height,
                                          frame->stride, frame->format,
                                          modifier, frame->fb_id,
                                          &egl_data, &egl_size);
        drmtap_debug_log(ctx, "EGL convert: ret=%d data=%p", ret, egl_data);
        if (ret == 0 && egl_data) {
            /* egl_data IS ctx->deswizzle_buf: the convert reads back into the
             * ctx-owned grow-once buffer (size-capped inside), so adopting it
             * is just repointing the frame — no allocation churn. */
            frame->data = egl_data;
            /* EGL outputs RGBA/BGRA 8-bit */
            frame->format = DRM_FORMAT_XRGB8888;
            frame->stride = frame->width * 4;
            drmtap_debug_log(ctx, "auto-process: EGL detiled to linear XRGB8888");
            return 0;
        }
        drmtap_debug_log(ctx, "auto-process: EGL failed (%d), trying CPU", ret);
    }
#endif

    /* A virgl readback was forced but the EGL path did not produce pixels (EGL
     * unavailable, or the import/readback failed). The only CPU-visible copy of
     * a host-rendered scanout is black, so fail closed instead of returning a
     * bogus frame as success. */
    if (force_egl) {
        drmtap_set_error(ctx,
            "virgl scanout needs GPU EGL readback, which is unavailable or failed");
        return -ENOTSUP;
    }

    /* The CPU deswizzle below reads from `data`. If we reached here with no CPU
     * mapping (a tiled scanout whose mmap failed, e.g. amdgpu GFX9+) and EGL did
     * not produce pixels, there is nothing to deswizzle — fail closed rather
     * than dereference a NULL source. */
    if (!data) {
        drmtap_set_error(ctx,
            "tiled scanout has no CPU mapping and EGL detile is unavailable");
        return -ENOTSUP;
    }

    /* An INVALID (unknown) modifier reaching this point means the EGL detile did
     * not run -- no dma-buf fd (helper V2 pixel mode) or EGL unavailable/failed.
     * INVALID is not a known tiling, so the CPU deswizzle below, which is written
     * for specific tiled layouts and hardcodes 4 bytes/pixel, cannot decode it and
     * would mangle a wider (FP16) scanout. Treat an unknown layout as linear -- its
     * practical case -- and reduce it from the RAW mapping, which handles 8/10/16-bit
     * correctly. This restores the pre-INVALID behavior for a flag-clear buffer that
     * used to read back as modifier 0, without diverting it into the tiled deswizzle. */
    if (modifier == DRM_FORMAT_MOD_INVALID) {
        int red = reduce_linear_to_xrgb8888(ctx, data, frame);
        if (red < 0) {
            return red;
        }
        /* red == 1: reduced to XRGB8888 (frame->data repointed). red == 0: already
         * 8-bit -- the raw linear mapping (frame->data, pre-set by the caller) stands. */
        return 0;
    }

    /* --- CPU deswizzle for classic tiling (buffer already allocated above) --- */
    const char *driver = ctx->driver_name;
    if (drmtap_gpu_intel_match(driver) ||
        drmtap_gpu_nvidia_match(driver) ||
        drmtap_gpu_amd_match(driver)) {
        drmtap_debug_log(ctx, "auto-process: %s CPU deswizzle (mod=0x%lx)",
                         driver, (unsigned long)modifier);
        int ret = drmtap_deswizzle(data, ctx->deswizzle_buf,
                                   frame->width, frame->height,
                                   frame->stride, frame->stride, modifier,
                                   (size_t)frame->stride * frame->height);
        if (ret == -ENOTSUP) {
            /* CCS-compressed modifier — CPU deswizzle impossible.
             * This happens in helper mode where dma_buf_fd is unavailable
             * and the pixel data came from dumb_mmap (still compressed).
             * Fall through to return raw data as-is with LINEAR modifier
             * so the caller doesn't crash trying to deswizzle. */
            drmtap_debug_log(ctx,
                "auto-process: CCS modifier 0x%lx needs GPU deswizzle "
                "(EGL/DMA-BUF), CPU deswizzle not possible — "
                "returning raw pixels",
                (unsigned long)modifier);
            frame->modifier = 0; /* DRM_FORMAT_MOD_LINEAR */
            return 0;
        }
        if (ret == 0) {
            frame->data = ctx->deswizzle_buf;
            drmtap_debug_log(ctx, "auto-process: CPU deswizzled to linear");

            /* Reduce 10-bit X/AR30 and X/AB30 to 8-bit XRGB8888. An HDR10 (PQ)
             * scanout needs a real tone-map -- the naive bit-shift would wash it
             * out; a plain SDR 10-bit scanout (same fourcc) just gets truncated.
             * Both converters read the fourcc and handle the RGB/BGR order. */
            if (frame->format == DRMTAP_FMT_XR30 ||
                frame->format == DRMTAP_FMT_AR30 ||
                frame->format == DRMTAP_FMT_XB30 ||
                frame->format == DRMTAP_FMT_AB30) {
                int conv;
                if (ctx->cur_hdr_eotf == DRMTAP_EOTF_PQ) {
                    drmtap_debug_log(ctx,
                        "auto-process: HDR10 AR30 -> tone-map to SDR (peak=%u)",
                        ctx->cur_hdr_max_nits);
                    conv = drmtap_tonemap_hdr10(
                        ctx->deswizzle_buf, ctx->deswizzle_buf,
                        frame->width, frame->height,
                        frame->stride, frame->stride,
                        frame->format, ctx->cur_hdr_max_nits);
                } else {
                    drmtap_debug_log(ctx,
                        "auto-process: SDR 10-bit AR30 -> 8-bit XRGB8888");
                    conv = drmtap_convert_format(
                        ctx->deswizzle_buf, ctx->deswizzle_buf,
                        frame->width, frame->height,
                        frame->stride, frame->stride,
                        frame->format, DRMTAP_FMT_XR24);
                }
                /* Only relabel the frame as 8-bit if the conversion succeeded;
                 * otherwise leave the original format so the caller doesn't read
                 * unconverted 10-bit data as XRGB8888. */
                if (conv != 0) {
                    return conv;
                }
                frame->format = DRMTAP_FMT_XR24;
            }
        }
        return ret;
    }

    drmtap_debug_log(ctx, "auto-process: unknown driver '%s' mod=0x%lx",
                     driver, (unsigned long)modifier);
    return 0;
}

/* ========================================================================= */
/* Public API                                                                */
/* ========================================================================= */

int drmtap_grab(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    if (!ctx || !frame) {
        return -EINVAL;
    }
    return do_grab(ctx, frame, 0);  /* zero-copy: DMA-BUF fd only */
}

int drmtap_grab_desc(drmtap_ctx *ctx, drmtap_dmabuf_desc *desc,
                     drmtap_frame_info *frame) {
    if (!ctx || !desc || !frame) {
        return -EINVAL;
    }
    int ret = do_grab(ctx, frame, 0);  /* zero-copy: DMA-BUF fd + metadata */
    if (ret != 0) {
        return ret;
    }
    /* The split model ships the dma_buf_fd to another process, so a grab that
     * produced pixels instead of a transferable fd (the helper V2 pixel
     * fallback sets frame->dma_buf_fd = -1) yields a descriptor the receiver
     * cannot convert — its fb_id is not valid across the process boundary
     * either. Fail closed rather than hand back an untransferable descriptor. */
    if (frame->dma_buf_fd < 0) {
        drmtap_set_error(ctx,
            "grab_desc needs a transferable DMA-BUF fd; this capture path "
            "returned pixels only (no exportable dma-buf)");
        drmtap_frame_release(ctx, frame);
        return -ENOTSUP;
    }
    /* Snapshot the full descriptor. The plane layout and HDR state are cached
     * on ctx during do_grab (from GetFB2 and the connector metadata) — they are
     * NOT in frame_info, which is exactly why a split exporter needs this call
     * rather than drmtap_grab alone. */
    memset(desc, 0, sizeof(*desc));
    desc->dma_buf_fd = frame->dma_buf_fd;
    desc->width = frame->width;
    desc->height = frame->height;
    desc->format = frame->format;
    desc->modifier = frame->modifier;
    desc->fb_id = frame->fb_id;

    int np = ctx->fb2_num_planes > 0 ? ctx->fb2_num_planes : 1;
    if (np > 4) {
        np = 4;
    }
    desc->num_planes = (uint32_t)np;
    for (int p = 0; p < 4; p++) {
        desc->offsets[p] = ctx->fb2_offsets[p];
        desc->pitches[p] = ctx->fb2_pitches[p];
    }
    /* Guarantee the main-surface stride is set even if GetFB2 left pitches[0]
     * zero (helper/dumb paths): fall back to the frame stride. */
    if (desc->pitches[0] == 0) {
        desc->pitches[0] = frame->stride;
    }
    desc->hdr_eotf = ctx->cur_hdr_eotf;
    desc->hdr_max_nits = ctx->cur_hdr_max_nits;
    return 0;
}

int drmtap_grab_mapped(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    if (!ctx || !frame) {
        return -EINVAL;
    }
    return do_grab(ctx, frame, 1);  /* mapped: mmap'd pixel data */
}

void drmtap_frame_release(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    if (!frame) {
        return;
    }

    frame_priv_t *priv = (frame_priv_t *)frame->_priv;
    if (priv) {
        /* Free or unmap pixel buffer */
        if (priv->mapped && priv->mapped_size > 0) {
            if (priv->is_heap_buf) {
                free(priv->mapped);
            } else if (priv->mapped != MAP_FAILED) {
                if (priv->sync_started && priv->prime_fd >= 0) {
                    dmabuf_sync_end(priv->prime_fd);
                }
                munmap(priv->mapped, priv->mapped_size);
            }
        }

        /* Close DMA-BUF fd */
        if (priv->prime_fd >= 0) {
            close(priv->prime_fd);
        }

        /* Close helper DRM fd */
        if (priv->helper_drm_fd >= 0) {
            close(priv->helper_drm_fd);
        }

        /* Close the GEM handle drmModeGetFB2 handed us on the direct path
         * (the helper path leaves it 0). */
        drmtap_gem_close(ctx, priv->gem_handle);

        free(priv);
    }

    /* Zero out the frame to prevent double-free */
    memset(frame, 0, sizeof(*frame));
    frame->dma_buf_fd = -1;
}

/* ========================================================================= */
/* Fast persistent-mmap capture (double-buffer cache)                         */
/* ========================================================================= */

// Clean up all cached buffer slots
void drmtap_fast_cleanup(drmtap_ctx *ctx) {
    for (int i = 0; i < DRMTAP_FAST_SLOTS; i++) {
        if (ctx->fast_slots[i].mmap_ptr &&
            ctx->fast_slots[i].mmap_ptr != MAP_FAILED) {
            munmap(ctx->fast_slots[i].mmap_ptr, ctx->fast_slots[i].mmap_size);
        }
        if (ctx->fast_slots[i].prime_fd >= 0) {
            close(ctx->fast_slots[i].prime_fd);
        }
        drmtap_gem_close(ctx, ctx->fast_slots[i].gem_handle);
        memset(&ctx->fast_slots[i], 0, sizeof(ctx->fast_slots[i]));
        ctx->fast_slots[i].prime_fd = -1;
    }
    ctx->fast_plane_id = 0;
    ctx->fast_last_fb_id = 0;
    ctx->fast_initialized = 0;
}

// Find or allocate a slot for the given fb_id
// Returns slot index, or -1 if cache is full (evicts LRU in that case)
static int find_or_alloc_slot(drmtap_ctx *ctx, uint32_t fb_id) {
    // Check if already cached
    for (int i = 0; i < DRMTAP_FAST_SLOTS; i++) {
        if (ctx->fast_slots[i].fb_id == fb_id) {
            return i;
        }
    }
    // Find empty slot
    for (int i = 0; i < DRMTAP_FAST_SLOTS; i++) {
        if (ctx->fast_slots[i].fb_id == 0) {
            return i;
        }
    }
    // Cache full — evict slot 0 (oldest)
    if (ctx->fast_slots[0].mmap_ptr &&
        ctx->fast_slots[0].mmap_ptr != MAP_FAILED) {
        munmap(ctx->fast_slots[0].mmap_ptr, ctx->fast_slots[0].mmap_size);
    }
    if (ctx->fast_slots[0].prime_fd >= 0) {
        close(ctx->fast_slots[0].prime_fd);
    }
    drmtap_gem_close(ctx, ctx->fast_slots[0].gem_handle);
    memset(&ctx->fast_slots[0], 0, sizeof(ctx->fast_slots[0]));
    ctx->fast_slots[0].prime_fd = -1;
    return 0;
}

/* Restage a cached slot's captured plane layout into ctx->fb2_*. A fast-path
 * cache HIT skips GetFB2, so without this ctx->fb2_* still describes whichever
 * fb was last GetFB2'd — and the EGL detile / CCS import (plus the EGLImage
 * cache's geometry compare) reads ctx->fb2_*. Restaging the slot's own layout
 * keeps the plane metadata matched to the frame actually being converted. */
static void fast_slot_restage_planes(drmtap_ctx *ctx, int slot) {
    ctx->fb2_num_planes = ctx->fast_slots[slot].fb2_num_planes;
    for (int p = 0; p < 4; p++) {
        ctx->fb2_pitches[p] = ctx->fast_slots[slot].fb2_pitches[p];
        ctx->fb2_offsets[p] = ctx->fast_slots[slot].fb2_offsets[p];
    }
}

int drmtap_grab_mapped_fast(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    if (!ctx || !frame) {
        return -EINVAL;
    }
    if (ctx->is_render_only) {
        drmtap_set_error(ctx, "context is render-only (drmtap_open_render); "
                         "grab needs a KMS context from drmtap_open");
        return -ENOTSUP;
    }

    int ret;

    /* Step 1: Find plane on first call only */
    if (!ctx->fast_initialized) {
        ctx->fast_plane_id = find_primary_plane(ctx);
        if (ctx->fast_plane_id == 0) {
            drmtap_set_error(ctx, "No active plane found for capture");
            return -ENODEV;
        }
        for (int i = 0; i < DRMTAP_FAST_SLOTS; i++) {
            memset(&ctx->fast_slots[i], 0, sizeof(ctx->fast_slots[i]));
            ctx->fast_slots[i].prime_fd = -1;
        }
        ctx->fast_last_fb_id = 0;
        ctx->fast_initialized = 1;
        drmtap_debug_log(ctx, "fast2: initialized plane=%u", ctx->fast_plane_id);
    }

    /* Step 2: Refresh fb_id (cheap ioctl, ~0.05ms) */
    drmModePlane *plane = drmModeGetPlane(ctx->drm_fd, ctx->fast_plane_id);
    if (!plane || plane->fb_id == 0) {
        if (plane) drmModeFreePlane(plane);
        drmtap_fast_cleanup(ctx);
        return -ENODEV;
    }
    uint32_t fb_id = plane->fb_id;
    drmModeFreePlane(plane);

    /* Step 3: If fb_id unchanged, use cached slot but ALWAYS re-transfer.
     * This gives us X11-style "always current" pixels with only 1 ioctl
     * instead of the 7 syscalls of grab_mapped. */
    if (fb_id == ctx->fast_last_fb_id) {
        for (int i = 0; i < DRMTAP_FAST_SLOTS; i++) {
            if (ctx->fast_slots[i].fb_id == fb_id && ctx->fast_slots[i].mmap_ptr) {
                /* Re-transfer to get current pixels */
                if (is_virtio_gpu(ctx)) {
                    ret = virtio_transfer_from_host(ctx,
                            ctx->fast_slots[i].gem_handle,
                            ctx->fast_slots[i].width,
                            ctx->fast_slots[i].height);
                    if (ret < 0) return ret;
                } else {
                    dmabuf_sync_start(ctx->fast_slots[i].prime_fd);
                }
                frame->data = ctx->fast_slots[i].mmap_ptr;
                frame->dma_buf_fd = ctx->fast_slots[i].prime_fd;
                frame->width = ctx->fast_slots[i].width;
                frame->height = ctx->fast_slots[i].height;
                frame->stride = ctx->fast_slots[i].stride;
                frame->format = ctx->fast_slots[i].format;
                frame->modifier = ctx->fast_slots[i].modifier;
                frame->fb_id = fb_id;
                frame->_priv = NULL;
                /* Restage this slot's plane layout before converting (the
                 * cache HIT skipped GetFB2, so ctx->fb2_* is otherwise stale). */
                fast_slot_restage_planes(ctx, i);
                /* Deswizzle/format-convert like the acquire path does — the
                 * cached mmap is the raw scanout, so a tiled buffer must be
                 * detiled here too (no-op for a linear one). Without this the
                 * unchanged-fb fast path returns raw tiled pixels. Propagate a
                 * processing failure instead of returning unprocessed pixels. */
                int pr = gpu_auto_process(ctx, frame->data, frame, 0);
                if (pr != 0) {
                    return pr;
                }
                return 0;   /* always return as new frame */
            }
        }
        /* Slot not found — fall through to acquire */
    }

    /* Step 4: fb_id changed — check if we have this buffer cached */
    int slot = find_or_alloc_slot(ctx, fb_id);

    if (ctx->fast_slots[slot].fb_id == fb_id && ctx->fast_slots[slot].mmap_ptr) {
        /* ═══ CACHE HIT ═══ Buffer already mapped from a previous flip!
         * Skip: GetFB2, PrimeHandleToFD, mmap  (saves ~4ms)
         * Only do: TRANSFER_FROM_HOST (the unavoidable part) */
        drmtap_debug_log(ctx, "fast2: CACHE HIT fb=%u slot=%d gem=%u",
                         fb_id, slot, ctx->fast_slots[slot].gem_handle);

        if (is_virtio_gpu(ctx)) {
            ret = virtio_transfer_from_host(ctx, ctx->fast_slots[slot].gem_handle,
                                             ctx->fast_slots[slot].width,
                                             ctx->fast_slots[slot].height);
            if (ret < 0) {
                drmtap_debug_log(ctx, "fast2: cached transfer failed: %d", ret);
                return ret;
            }
        } else {
            dmabuf_sync_start(ctx->fast_slots[slot].prime_fd);
        }

        ctx->fast_last_fb_id = fb_id;

        frame->data = ctx->fast_slots[slot].mmap_ptr;
        frame->dma_buf_fd = ctx->fast_slots[slot].prime_fd;
        frame->width = ctx->fast_slots[slot].width;
        frame->height = ctx->fast_slots[slot].height;
        frame->stride = ctx->fast_slots[slot].stride;
        frame->format = ctx->fast_slots[slot].format;
        frame->modifier = ctx->fast_slots[slot].modifier;
        frame->fb_id = fb_id;
        frame->_priv = NULL;

        /* Restage this slot's plane layout before converting (the cache HIT
         * skipped GetFB2, so ctx->fb2_* is otherwise stale). */
        fast_slot_restage_planes(ctx, slot);
        /* Auto-deswizzle tiled framebuffers + format convert. Propagate a
         * processing failure instead of returning unprocessed pixels as success
         * (the unchanged-fb branch above already does this). */
        int pr = gpu_auto_process(ctx, frame->data, frame, 0);
        if (pr != 0) {
            return pr;
        }

        return 0;   /* new frame */
    }

    /* ═══ CACHE MISS ═══ First time seeing this fb_id — full setup */
    drmtap_debug_log(ctx, "fast2: CACHE MISS fb=%u slot=%d (cold start)",
                     fb_id, slot);

    drmModeFB2 *fb2 = drmModeGetFB2(ctx->drm_fd, fb_id);
    if (!fb2) {
        ret = -errno;
        drmtap_set_error(ctx, "fast2: drmModeGetFB2(%u) failed: %s",
                         fb_id, strerror(errno));
        return ret;
    }

    if (fb2->handles[0] == 0) {
        drmModeFreeFB2(fb2);
        drmtap_set_error(ctx, "fast2: handles[0]==0, no CAP_SYS_ADMIN");
        return -EACCES;
    }

    ret = validate_fb_size(fb2->pitches[0], fb2->height);
    if (ret != 0) {
        drmtap_set_error(ctx, "fast2: rejecting geometry %ux%u stride=%u",
                         fb2->width, fb2->height, fb2->pitches[0]);
        drmtap_gem_close(ctx, fb2->handles[0]);
        drmModeFreeFB2(fb2);
        return ret;
    }

    /* Cache multi-plane info for the EGL CCS import. The EGL path reads plane
     * offsets/pitches from ctx->fb2_*, which otherwise still holds whatever
     * the last do_grab() left there (stale geometry from another fb). */
    ctx->fb2_num_planes = 0;
    for (int p = 0; p < 4; p++) {
        ctx->fb2_pitches[p] = fb2->pitches[p];
        ctx->fb2_offsets[p] = fb2->offsets[p];
        if (fb2->handles[p] || fb2->pitches[p]) {
            ctx->fb2_num_planes = p + 1;
        }
    }

    /* Export DMA-BUF */
    int prime_fd = -1;
    ret = drmPrimeHandleToFD(ctx->drm_fd, fb2->handles[0],
                              O_RDONLY | O_CLOEXEC, &prime_fd);
    if (ret < 0) {
        ret = -errno;
        drmtap_gem_close(ctx, fb2->handles[0]);
        drmModeFreeFB2(fb2);
        return ret;
    }

    /* mmap */
    size_t size = (size_t)fb2->pitches[0] * fb2->height;
    void *mapped = MAP_FAILED;

    if (is_virtio_gpu(ctx)) {
        ret = virtio_transfer_from_host(ctx, fb2->handles[0],
                                         fb2->width, fb2->height);
        if (ret >= 0) {
            mapped = virtio_dumb_mmap(ctx, fb2->handles[0], size);
        }
    }

    if (mapped == MAP_FAILED) {
        dmabuf_sync_start(prime_fd);
        mapped = mmap(NULL, size, PROT_READ, MAP_SHARED,
                      prime_fd, fb2->offsets[0]);
    }

    /* Test hook (drmtap_force_mmap_fail): drop a successful mapping so the EGL fd
     * fallback below is exercised on a GPU whose scanout IS cpu-mappable. */
    if (mapped != MAP_FAILED && drmtap_force_mmap_fail()) {
        munmap(mapped, size);
        mapped = MAP_FAILED;
    }

    if (mapped == MAP_FAILED) {
#ifdef HAVE_EGL
        /* A tiled scanout can refuse a CPU mmap (amdgpu GFX9+, discrete VRAM,
         * nvidia) yet be fully capturable by EGL-detiling the exported fd, exactly
         * as do_grab does. Fall back to that instead of dropping the capture. This
         * frame has no CPU mapping to cache, so no slot is stored; the fd path
         * re-exports each frame (still cheaper than failing the stream, and the EGL
         * image cache keyed on the BO inode keeps the import itself amortized). */
        if (drmtap_gpu_egl_available(ctx)) {
            frame->data = NULL;
            frame->dma_buf_fd = prime_fd;
            frame->width = fb2->width;
            frame->height = fb2->height;
            frame->stride = fb2->pitches[0];
            frame->format = fb2->pixel_format;
            frame->modifier = (fb2->flags & DRM_MODE_FB_MODIFIERS)
                                  ? fb2->modifier : DRM_FORMAT_MOD_INVALID;
            frame->fb_id = fb_id;
            frame->_priv = NULL;
            int pr = gpu_auto_process(ctx, NULL, frame, 0);
            /* A LINEAR-modifier scanout takes gpu_auto_process's linear early-return,
             * which leaves frame->data NULL when it was called with data==NULL (no CPU
             * mapping): that path assumes the caller already pointed frame->data at the
             * raw mapping, which we do not have here. Treat a 0-return with no data as a
             * failure so the caller never gets a success frame with a NULL buffer
             * (do_grab guards the same case). Only EGL actually produces pixels here. */
            if (pr == 0 && !frame->data) {
                pr = -EIO;
            }
            /* gpu_auto_process EGL-read the fd back into ctx->deswizzle_buf, so the
             * fd and handle are no longer needed and nothing is cached for fb_id. */
            close(prime_fd);
            frame->dma_buf_fd = -1;  /* prime_fd is closed; don't hand back a stale fd */
            drmtap_gem_close(ctx, fb2->handles[0]);
            drmModeFreeFB2(fb2);
            ctx->fast_last_fb_id = 0;  /* uncached: next call does a fresh setup */
            if (pr != 0) {
                return pr;
            }
            return 0;
        }
#endif
        close(prime_fd);
        drmtap_gem_close(ctx, fb2->handles[0]);
        drmModeFreeFB2(fb2);
        return -ENOMEM;
    }

    /* Store in slot */
    ctx->fast_slots[slot].fb_id = fb_id;
    ctx->fast_slots[slot].gem_handle = fb2->handles[0];
    ctx->fast_slots[slot].prime_fd = prime_fd;
    ctx->fast_slots[slot].mmap_ptr = mapped;
    ctx->fast_slots[slot].mmap_size = size;
    ctx->fast_slots[slot].width = fb2->width;
    ctx->fast_slots[slot].height = fb2->height;
    ctx->fast_slots[slot].stride = fb2->pitches[0];
    ctx->fast_slots[slot].format = fb2->pixel_format;
    /* Honor the DRM_MODE_FB_MODIFIERS flag (see do_grab): fb2->modifier is undefined
     * when the flag is clear, so a tiled scanout that omits it must be cached as
     * DRM_FORMAT_MOD_INVALID -- not the bogus 0 -- or the cached slot would replay it
     * as linear on every fast-path hit and corrupt the XR30/tiled class this fixes. */
    ctx->fast_slots[slot].modifier = (fb2->flags & DRM_MODE_FB_MODIFIERS)
                                         ? fb2->modifier
                                         : DRM_FORMAT_MOD_INVALID;
    /* Capture this fb's plane layout with the slot so a later cache HIT can
     * restage it into ctx->fb2_* (ctx->fb2_* was set from this same fb2 just
     * above, at the "Cache multi-plane info" block). */
    ctx->fast_slots[slot].fb2_num_planes = ctx->fb2_num_planes;
    for (int p = 0; p < 4; p++) {
        ctx->fast_slots[slot].fb2_pitches[p] = ctx->fb2_pitches[p];
        ctx->fast_slots[slot].fb2_offsets[p] = ctx->fb2_offsets[p];
    }

    drmtap_debug_log(ctx, "fast2: cached fb=%u slot=%d gem=%u %ux%u",
                     fb_id, slot, fb2->handles[0], fb2->width, fb2->height);

    drmModeFreeFB2(fb2);
    ctx->fast_last_fb_id = fb_id;

    /* Fill frame info */
    frame->data = mapped;
    frame->dma_buf_fd = prime_fd;
    frame->width = ctx->fast_slots[slot].width;
    frame->height = ctx->fast_slots[slot].height;
    frame->stride = ctx->fast_slots[slot].stride;
    frame->format = ctx->fast_slots[slot].format;
    frame->modifier = ctx->fast_slots[slot].modifier;
    frame->fb_id = fb_id;
    frame->_priv = NULL;

    /* Auto-deswizzle tiled framebuffers + format convert. Propagate a processing
     * failure instead of returning unprocessed pixels as success. */
    int mpr = gpu_auto_process(ctx, frame->data, frame, 0);
    if (mpr != 0) {
        return mpr;
    }

    return 0;   /* new frame */
}

/* ========================================================================= */
/* Split capture: unprivileged convert of an externally-supplied DMA-BUF     */
/* ========================================================================= */

/* Minimum bytes-per-pixel of a scanout fourcc, for the width*bpp <= stride
 * bound. The CPU deswizzle/reduce paths index the row as pixel[x] for
 * x < width, so a stride that does not cover width*bpp would read/write past
 * the row. 8-bit and 10-bit RGB pack into 4 bytes; the 16-bit-per-channel
 * formats into 8. Unknown fourccs default to 4: a real desktop scanout is
 * never sub-4-byte, and the tiled deswizzle writes 4-byte pixels, so 4 is the
 * safe floor to reject a hostile narrow stride. */
static uint32_t format_min_bpp(uint32_t fourcc) {
    switch (fourcc) {
    case DRMTAP_FMT_XR48:
    case DRMTAP_FMT_AR48:
    case DRMTAP_FMT_XB48:
    case DRMTAP_FMT_AB48:
        return 8;
    default:
        return 4;
    }
}

int drmtap_convert_dmabuf(drmtap_ctx *ctx, const drmtap_dmabuf_desc *desc,
                          drmtap_frame_info *frame) {
    if (!ctx || !desc || !frame) {
        return -EINVAL;
    }

    uint32_t num_planes = desc->num_planes ? desc->num_planes : 1;
    if (num_planes > 4 || desc->width == 0) {
        drmtap_set_error(ctx, "convert: invalid descriptor "
                         "(width=%u num_planes=%u)",
                         desc->width, desc->num_planes);
        return -EINVAL;
    }
    int ret = validate_fb_size(desc->pitches[0], desc->height);
    if (ret != 0) {
        drmtap_set_error(ctx, "convert: rejecting geometry %ux%u stride=%u",
                         desc->width, desc->height, desc->pitches[0]);
        return ret;
    }
    /* The descriptor comes from the (untrusted) exporter over IPC, unlike the
     * in-process grab whose width/stride come from drmModeGetFB2. validate_fb_size
     * bounds stride*height but is independent of width — enforce that the row
     * stride actually covers width pixels so the per-pixel CPU converters below
     * (and the EGL import) cannot be driven past the buffer. */
    if ((uint64_t)desc->width * format_min_bpp(desc->format) > desc->pitches[0]) {
        drmtap_set_error(ctx, "convert: stride %u too small for width %u "
                         "(fourcc %.4s)", desc->pitches[0], desc->width,
                         (const char *)&desc->format);
        return -EINVAL;
    }

    /* Stage the plane + HDR metadata exactly where the in-process grab
     * (drmModeGetFB2 + the connector HDR read) would put it — the conversion
     * paths below consume both from the context. */
    ctx->fb2_num_planes = (int)num_planes;
    for (uint32_t p = 0; p < 4; p++) {
        ctx->fb2_pitches[p] = (p < num_planes) ? desc->pitches[p] : 0;
        ctx->fb2_offsets[p] = (p < num_planes) ? desc->offsets[p] : 0;
    }
    ctx->cur_hdr_eotf = desc->hdr_eotf;
    ctx->cur_hdr_max_nits = desc->hdr_max_nits;

    memset(frame, 0, sizeof(*frame));
    frame->width = desc->width;
    frame->height = desc->height;
    frame->stride = desc->pitches[0];
    frame->format = desc->format;
    frame->modifier = desc->modifier;
    frame->fb_id = desc->fb_id;
    frame->dma_buf_fd = desc->dma_buf_fd;

    /* The descriptor and its fd cross an IPC boundary and are UNTRUSTED. Validate
     * the fd BEFORE either conversion path touches it: the EGL import below hands
     * width/height/stride/offset straight to eglCreateImage with no size check, so
     * the 0.4.12 fd-type + size bound (previously only in the CPU fallback) must
     * gate the EGL path too. Only meaningful when an fd is supplied; a cached-fb_id
     * EGL import carries no untrusted fd. Gates:
     *   1. Require a genuine DMA-BUF -- immutable in size, so it cannot be shrunk
     *      between check and use (a memfd/regular file could, faulting the reader).
     *      dmabuf_sync returns ENOTTY on a non-dma-buf; sync_end closes the probe.
     *   2. Bound offsets[0] + pitches[0]*height against the real fd size; fail
     *      CLOSED when the size cannot be determined. */
    if (desc->dma_buf_fd >= 0) {
        size_t need = (size_t)desc->pitches[0] * desc->height;
        if (dmabuf_sync_start(desc->dma_buf_fd) != 0) {
            drmtap_set_error(ctx, "convert: fd is not a dma-buf");
            return -EINVAL;
        }
        off_t fd_size = lseek(desc->dma_buf_fd, 0, SEEK_END);
        if (fd_size <= 0) {
            struct stat st;
            if (fstat(desc->dma_buf_fd, &st) == 0) {
                fd_size = st.st_size;
            }
        }
        dmabuf_sync_end(desc->dma_buf_fd);
        if (fd_size <= 0 ||
            (uint64_t)desc->offsets[0] + (uint64_t)need > (uint64_t)fd_size) {
            drmtap_set_error(ctx, "convert: descriptor exceeds dma-buf size "
                             "(fd_size=%lld, need offset %u + %zu)",
                             (long long)fd_size, desc->offsets[0], need);
            return -EINVAL;
        }
    }

#ifdef HAVE_EGL
    /* GPU path first: EGL imports the fd (or reuses the fb_id-cached import)
     * and hands back linear XRGB8888 — one hop covers every vendor tiling,
     * the 10/16-bit reductions and the HDR tone-map. */
    if (drmtap_gpu_egl_available(ctx)) {
        void *egl_data = NULL;
        size_t egl_size = 0;
        ret = drmtap_gpu_egl_convert(ctx, desc->dma_buf_fd,
                                     desc->width, desc->height,
                                     desc->pitches[0], desc->format,
                                     desc->modifier, desc->fb_id,
                                     &egl_data, &egl_size);
        if (ret == 0 && egl_data) {
            frame->data = egl_data;   /* ctx-owned grow-once buffer */
            frame->format = DRM_FORMAT_XRGB8888;
            frame->stride = desc->width * 4;
            frame->modifier = DRM_FORMAT_MOD_LINEAR;
            return 0;
        }
        drmtap_debug_log(ctx, "convert: EGL path failed (%d), CPU fallback",
                         ret);
    }
#endif

    /* CPU fallback: map the DMA-BUF read-only and run the same deswizzle /
     * format-reduction pipeline the in-process grab uses. */
    if (desc->dma_buf_fd < 0) {
        drmtap_set_error(ctx, "convert: fb_id %u is not cached and no "
                         "dma-buf fd was supplied", desc->fb_id);
        return -EINVAL;
    }
    size_t map_size = (size_t)desc->pitches[0] * desc->height;
    /* The descriptor and its fd cross an IPC boundary and are UNTRUSTED. Two
     * gates before we mmap and read the pixels:
     *
     * 1. Require a genuine DMA-BUF. It is the only legitimate input here, and it
     *    is immutable in size, so it CANNOT be shrunk between the size check and
     *    the read. A memfd / regular file could be ftruncate()d mid-read by a
     *    hostile peer, faulting the reader with SIGBUS (a check-then-use race).
     *    The DMA-BUF sync ioctl succeeds on every real dma-buf and returns
     *    ENOTTY on a non-dma-buf, so it gates the source and also serves as the
     *    CPU-access SYNC_START we need before reading.
     * 2. Bound the read against the buffer's real size, so a descriptor that
     *    claims more than the dma-buf backs is rejected instead of faulting.
     *    lseek(SEEK_END) reports the dma-buf size across kernels (fstat returns
     *    0 for a dma-buf before Linux 5.3, which would falsely reject every
     *    frame); fstat is only a fallback when lseek yields nothing. */
    if (dmabuf_sync_start(desc->dma_buf_fd) != 0) {
        drmtap_set_error(ctx, "convert: fd is not a DMA-BUF; refusing to mmap "
                         "an untrusted non-dma-buf source");
        return -EINVAL;
    }
    int sync_started = 1;
    off_t fd_size = lseek(desc->dma_buf_fd, 0, SEEK_END);
    if (fd_size <= 0) {
        struct stat st;
        if (fstat(desc->dma_buf_fd, &st) == 0 && st.st_size > 0) {
            fd_size = st.st_size;
        }
    }
    /* Fail CLOSED: if the size cannot be determined (fd_size <= 0, e.g. a dma-buf
     * whose backend supports neither llseek nor a per-inode fstat), reject
     * rather than mmap+read a buffer we cannot bound (which could fault). Every
     * real scanout dma-buf on a supported kernel reports its size via lseek, so
     * this rejects only pathological/unknown-size fds, never a legitimate frame. */
    if (fd_size <= 0 ||
        (uint64_t)desc->offsets[0] + (uint64_t)map_size > (uint64_t)fd_size) {
        dmabuf_sync_end(desc->dma_buf_fd);
        drmtap_set_error(ctx, "convert: dma-buf size unknown or too small for the "
                         "declared geometry (buffer %lld bytes, need offset %u "
                         "+ %zu)", (long long)fd_size, desc->offsets[0], map_size);
        return -EINVAL;
    }
    void *mapped = mmap(NULL, map_size, PROT_READ, MAP_SHARED,
                        desc->dma_buf_fd, desc->offsets[0]);
    if (mapped == MAP_FAILED) {
        int mmap_errno = errno;
        dmabuf_sync_end(desc->dma_buf_fd);
        drmtap_set_error(ctx, "convert: mmap(%zu) failed: %s "
                         "(and no usable EGL path)",
                         map_size, strerror(mmap_errno));
        return mmap_errno ? -mmap_errno : -EIO;
    }

    /* Hide the fd for the gpu_auto_process call so it stays off the EGL
     * branch it would otherwise retry (we just watched EGL fail, or it is
     * unavailable) and takes the CPU deswizzle/reduce paths on `mapped`. */
    frame->data = mapped;
    frame->dma_buf_fd = -1;
    ret = gpu_auto_process(ctx, mapped, frame, 0);
    frame->dma_buf_fd = desc->dma_buf_fd;

    if (ret == 0 && frame->data == mapped) {
        /* Linear 8-bit passthrough (no deswizzle / reduction happened). Copy
         * into the ctx buffer so the pixels survive the temporary mapping,
         * AND repack to a tight width*4 stride so the returned frame honors
         * the documented "stride width*4" output even when the source scanout
         * carried row padding (pitches[0] > width*4). The width*4 <= pitches[0]
         * bound checked above keeps every per-row read inside the mapping. */
        uint32_t out_stride = desc->width * 4u;
        size_t out_size = (size_t)out_stride * desc->height;
        ret = drmtap_ensure_buf(&ctx->deswizzle_buf, &ctx->deswizzle_buf_size,
                                out_size);
        if (ret == 0) {
            const uint8_t *src = mapped;
            uint8_t *dst = ctx->deswizzle_buf;
            if (desc->pitches[0] == out_stride) {
                memcpy(dst, src, out_size);
            } else {
                for (uint32_t y = 0; y < desc->height; y++) {
                    memcpy(dst + (size_t)y * out_stride,
                           src + (size_t)y * desc->pitches[0], out_stride);
                }
            }
            frame->data = ctx->deswizzle_buf;
            frame->stride = out_stride;
        }
    }

    if (sync_started) {
        dmabuf_sync_end(desc->dma_buf_fd);
    }
    munmap(mapped, map_size);

    if (ret == 0 && !frame->data) {
        drmtap_set_error(ctx, "convert: no conversion path produced pixels");
        ret = -ENOTSUP;
    }
    if (ret != 0) {
        frame->data = NULL;
    }
    return ret;
}
