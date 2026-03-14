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

/* ========================================================================= */
/* Internal state for a captured frame (stored in frame->_priv)              */
/* ========================================================================= */

typedef struct {
    int prime_fd;           /* DMA-BUF fd from PrimeHandleToFD */
    void *mapped;           /* mmap'd pointer (or MAP_FAILED) */
    size_t mapped_size;     /* size of mapped region */
    uint32_t gem_handle;    /* GEM handle (needs close) */
    int used_dumb_map;      /* 1 if mapped via dumb buffer (virtio_gpu) */
} frame_priv_t;

/* ========================================================================= */
/* Plane discovery                                                           */
/* ========================================================================= */

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

    /* Get CRTC index for pipe matching */
    uint32_t crtc_index = 0;
    drmModeRes *res = drmModeGetResources(ctx->drm_fd);
    if (res) {
        for (int i = 0; i < res->count_crtcs; i++) {
            if (res->crtcs[i] == target_crtc) {
                crtc_index = (uint32_t)i;
                break;
            }
        }
        drmModeFreeResources(res);
    }

    /* Search for primary plane on the target CRTC */
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *plane = drmModeGetPlane(ctx->drm_fd, planes->planes[i]);
        if (!plane) {
            continue;
        }

        /* Check if this plane can drive our CRTC */
        if (!(plane->possible_crtcs & (1u << crtc_index))) {
            drmModeFreePlane(plane);
            continue;
        }

        /* Check if it has an active framebuffer */
        if (plane->fb_id != 0) {
            /* Check plane type to prefer PRIMARY */
            drmModeObjectProperties *props = drmModeObjectGetProperties(
                ctx->drm_fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
            int is_primary = 0;

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
                drmtap_debug_log(ctx, "found plane %u (fb=%u, %s)",
                                 plane->plane_id, plane->fb_id,
                                 is_primary ? "PRIMARY" : "overlay");
            }
        }

        drmModeFreePlane(plane);
        if (result != 0 && planes->count_planes > 1) {
            /* Keep looking for PRIMARY even if we found an overlay */
        }
    }

    drmModeFreePlaneResources(planes);
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

// Internal capture that populates frame_info
// If do_mmap is true, also maps the pixel data to frame->data
static int do_grab(drmtap_ctx *ctx, drmtap_frame_info *frame, int do_mmap) {
    int ret;
    frame_priv_t *priv = NULL;
    drmModeFB2 *fb2 = NULL;
    int prime_fd = -1;

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

    /* Step 4: Check handles[0] — if 0, we lack CAP_SYS_ADMIN */
    if (fb2->handles[0] == 0) {
        drmtap_set_error(ctx,
            "FB handles[0]==0: missing CAP_SYS_ADMIN. "
            "Run with sudo or configure the drmtap-helper binary.");
        drmModeFreeFB2(fb2);
        return -EACCES;
    }

    /* Step 5: Export as DMA-BUF fd */
    ret = drmPrimeHandleToFD(ctx->drm_fd, fb2->handles[0],
                             O_RDONLY | O_CLOEXEC, &prime_fd);
    if (ret < 0) {
        ret = -errno;
        drmtap_set_error(ctx, "drmPrimeHandleToFD failed: %s",
                         strerror(errno));
        goto cleanup;
    }

    /* Allocate private state */
    priv = calloc(1, sizeof(frame_priv_t));
    if (!priv) {
        ret = -ENOMEM;
        goto cleanup;
    }
    priv->prime_fd = prime_fd;
    priv->gem_handle = fb2->handles[0];
    priv->mapped = MAP_FAILED;
    priv->mapped_size = 0;

    /* Fill frame info */
    memset(frame, 0, sizeof(*frame));
    frame->width = fb2->width;
    frame->height = fb2->height;
    frame->stride = fb2->pitches[0];
    frame->format = fb2->pixel_format;
    frame->modifier = fb2->modifier;
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
            drmtap_set_error(ctx, "mmap failed (%zu bytes): %s",
                             size, strerror(errno));
            drmtap_debug_log(ctx, "mmap failed, falling back to zero-copy");
            frame->data = NULL;
        } else {
            priv->mapped = mapped;
            priv->mapped_size = size;
            frame->data = mapped;
            drmtap_debug_log(ctx, "mapped %zu bytes at %p", size, mapped);
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
    free(priv);
    return ret;
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

int drmtap_grab_mapped(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    if (!ctx || !frame) {
        return -EINVAL;
    }
    return do_grab(ctx, frame, 1);  /* mapped: mmap'd pixel data */
}

void drmtap_frame_release(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    (void)ctx;

    if (!frame) {
        return;
    }

    frame_priv_t *priv = (frame_priv_t *)frame->_priv;
    if (priv) {
        /* Unmap if mapped */
        if (priv->mapped != MAP_FAILED && priv->mapped_size > 0) {
            dmabuf_sync_end(priv->prime_fd);
            munmap(priv->mapped, priv->mapped_size);
        }

        /* Close DMA-BUF fd */
        if (priv->prime_fd >= 0) {
            close(priv->prime_fd);
        }

        /* Close GEM handle to avoid leaks */
        /* Note: GEM handles are per-process, closed via DRM ioctl */
        /* The handle was from drmModeGetFB2, no explicit close needed
         * as the fd close handles it */

        free(priv);
    }

    /* Zero out the frame to prevent double-free */
    memset(frame, 0, sizeof(*frame));
    frame->dma_buf_fd = -1;
}
