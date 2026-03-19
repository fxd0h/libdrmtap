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
#include <drm_fourcc.h>

/* Forward declaration */
static int gpu_auto_process(drmtap_ctx *ctx, void *data,
                            drmtap_frame_info *frame);

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
            needs_helper = 1;
        } else if (ret < 0) {
            ret = -errno;
            drmtap_set_error(ctx, "drmPrimeHandleToFD failed: %s", strerror(errno));
            goto cleanup;
        }
    }

    if (needs_helper) {
        drmtap_debug_log(ctx,
            "No CAP_SYS_ADMIN (needs helper), trying helper...");

        /* Allocate pixel buffer for helper to fill */
        size_t buf_size = (size_t)fb2->pitches[0] * fb2->height;
        void *pixel_buf = malloc(buf_size);
        if (!pixel_buf) {
            drmModeFreeFB2(fb2);
            return -ENOMEM;
        }

        /* Helper reads pixels in its own process and sends via socket */
        helper_grab_result_t hresult;
        ret = drmtap_helper_grab(ctx, &hresult, pixel_buf, buf_size);
        if (ret < 0) {
            free(pixel_buf);
            drmtap_set_error(ctx,
                "No CAP_SYS_ADMIN and helper failed (ret=%d). "
                "Install the helper: sudo cp drmtap-helper /usr/local/bin/ "
                "&& sudo setcap cap_sys_admin+ep /usr/local/bin/drmtap-helper",
                ret);
            drmModeFreeFB2(fb2);
            return -EACCES;
        }

        /* Allocate private state */
        priv = calloc(1, sizeof(frame_priv_t));
        if (!priv) {
            free(pixel_buf);
            drmModeFreeFB2(fb2);
            return -ENOMEM;
        }
        priv->helper_drm_fd = -1;
        priv->prime_fd = -1;
        priv->gem_handle = 0;
        priv->mapped = pixel_buf;       /* reuse mapped field for free() */
        priv->mapped_size = buf_size;
        priv->used_dumb_map = 1;        /* skip dmabuf_sync in release */
        priv->is_heap_buf = 1;          /* free() instead of munmap() */

        /* Fill frame info from helper metadata */
        memset(frame, 0, sizeof(*frame));
        frame->width = hresult.width;
        frame->height = hresult.height;
        frame->stride = hresult.stride;
        frame->format = hresult.format;
        frame->modifier = hresult.modifier;
        frame->fb_id = hresult.fb_id;
        frame->dma_buf_fd = -1;
        frame->data = pixel_buf;
        frame->_priv = priv;

        if (do_mmap) {
            gpu_auto_process(ctx, pixel_buf, frame);
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
    frame->modifier = fb2->modifier;
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
            drmtap_set_error(ctx, "mmap failed (%zu bytes): %s",
                             size, strerror(errno));
            drmtap_debug_log(ctx, "mmap failed, falling back to zero-copy");
            frame->data = NULL;
        } else {
            /* Unconditionally invalidate CPU caches using SYNC_START */
            /* Crucial for virtio_gpu where the transfer arrives in system RAM asynchronously */
            dmabuf_sync_start(prime_fd);

            priv->mapped = mapped;
            priv->mapped_size = size;
            frame->data = mapped;
            drmtap_debug_log(ctx, "mapped %zu bytes at %p", size, mapped);

            /* Auto-deswizzle tiled framebuffers + format convert */
            gpu_auto_process(ctx, mapped, frame);
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
/* Auto-process: deswizzle + format convert based on GPU driver              */
/* ========================================================================= */

static int gpu_auto_process(drmtap_ctx *ctx, void *data,
                            drmtap_frame_info *frame) {
    if (!data) return 0;

    uint64_t modifier = frame->modifier;

    /* Linear framebuffer: no deswizzle needed */
    if (modifier == DRM_FORMAT_MOD_LINEAR || modifier == 0) {
        return 0;
    }

    /*
     * Non-linear (tiled) framebuffer detected.
     *
     * The mmap'd DMA-BUF is read-only (PROT_READ | MAP_SHARED), so we
     * always deswizzle into a separate buffer (ctx->deswizzle_buf).
     */

    /* Format conversion constants */
    #define DRM_FMT_XR30 0x30335258u  /* fourcc('X','R','3','0') */
    #define DRM_FMT_AR30 0x30335241u  /* fourcc('A','R','3','0') */
    #define DRM_FMT_XR24 0x34325258u  /* fourcc('X','R','2','4') */

    /* Ensure shadow buffer is allocated */
    size_t size = (size_t)frame->stride * frame->height;
    if (ctx->deswizzle_buf_size < size) {
        free(ctx->deswizzle_buf);
        ctx->deswizzle_buf = malloc(size);
        if (!ctx->deswizzle_buf) {
            ctx->deswizzle_buf_size = 0;
            return -ENOMEM;
        }
        ctx->deswizzle_buf_size = size;
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
    if (frame->dma_buf_fd >= 0 && drmtap_gpu_egl_available(ctx)) {
        void *egl_data = NULL;
        size_t egl_size = 0;
        drmtap_debug_log(ctx, "auto-process: EGL convert (mod=0x%lx)",
                         (unsigned long)modifier);
        int ret = drmtap_gpu_egl_convert(ctx, frame->dma_buf_fd,
                                          frame->width, frame->height,
                                          frame->stride, frame->format,
                                          modifier, &egl_data, &egl_size);
        if (ret == 0 && egl_data) {
            /* Store in ctx for reuse / cleanup */
            free(ctx->deswizzle_buf);
            ctx->deswizzle_buf = egl_data;
            ctx->deswizzle_buf_size = egl_size;
            frame->data = ctx->deswizzle_buf;
            /* EGL outputs RGBA/BGRA 8-bit */
            frame->format = DRM_FORMAT_XRGB8888;
            frame->stride = frame->width * 4;
            drmtap_debug_log(ctx, "auto-process: EGL detiled to linear XRGB8888");
            return 0;
        }
        drmtap_debug_log(ctx, "auto-process: EGL failed (%d), trying CPU", ret);
    }
#endif

    /* --- CPU deswizzle for classic tiling (buffer already allocated above) --- */
    const char *driver = ctx->driver_name;
    if (drmtap_gpu_intel_match(driver) ||
        drmtap_gpu_nvidia_match(driver) ||
        drmtap_gpu_amd_match(driver)) {
        drmtap_debug_log(ctx, "auto-process: %s CPU deswizzle (mod=0x%lx)",
                         driver, (unsigned long)modifier);
        int ret = drmtap_deswizzle(data, ctx->deswizzle_buf,
                                   frame->width, frame->height,
                                   frame->stride, frame->stride, modifier);
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

            /* Convert 10-bit XR30/AR30 → 8-bit XRGB8888 if needed */
            if (frame->format == DRM_FMT_XR30 ||
                frame->format == DRM_FMT_AR30) {
                drmtap_debug_log(ctx,
                    "auto-process: converting XR30 10-bit → XRGB8888 8-bit");
                drmtap_convert_format(
                    ctx->deswizzle_buf, ctx->deswizzle_buf,
                    frame->width, frame->height,
                    frame->stride, frame->stride,
                    frame->format, DRM_FMT_XR24);
                frame->format = DRM_FMT_XR24;
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
        /* Free or unmap pixel buffer */
        if (priv->mapped && priv->mapped_size > 0) {
            if (priv->is_heap_buf) {
                free(priv->mapped);
            } else if (priv->mapped != MAP_FAILED) {
                if (!priv->used_dumb_map && priv->prime_fd >= 0) {
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
    memset(&ctx->fast_slots[0], 0, sizeof(ctx->fast_slots[0]));
    ctx->fast_slots[0].prime_fd = -1;
    return 0;
}

int drmtap_grab_mapped_fast(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    if (!ctx || !frame) {
        return -EINVAL;
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

        /* Auto-deswizzle tiled framebuffers + format convert */
        gpu_auto_process(ctx, frame->data, frame);

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

    /* Export DMA-BUF */
    int prime_fd = -1;
    ret = drmPrimeHandleToFD(ctx->drm_fd, fb2->handles[0],
                              O_RDONLY | O_CLOEXEC, &prime_fd);
    if (ret < 0) {
        ret = -errno;
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

    if (mapped == MAP_FAILED) {
        close(prime_fd);
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
    ctx->fast_slots[slot].modifier = fb2->modifier;

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

    /* Auto-deswizzle tiled framebuffers + format convert */
    gpu_auto_process(ctx, frame->data, frame);

    return 0;   /* new frame */
}
