/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file cursor.c
 * @brief Cursor plane capture — position, image, and hotspot
 *
 * Reads the cursor plane from DRM/KMS to extract:
 *   - Cursor position (SRC_X, SRC_Y properties of the cursor plane)
 *   - Cursor image (framebuffer attached to the cursor plane)
 *   - Hotspot (HOTSPOT_X, HOTSPOT_Y properties, driver-dependent)
 *
 * Cursor data is returned separately from the main framebuffer so
 * remote desktop clients can render it locally for lower latency.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <linux/dma-buf.h>  /* struct dma_buf_sync, DMA_BUF_IOCTL_SYNC */

#include "drmtap_internal.h"

/* ========================================================================= */
/* Property helpers                                                          */
/* ========================================================================= */

// Get a property value by name from a DRM object
static int get_property_value(int fd, uint32_t object_id, uint32_t object_type,
                              const char *name, uint64_t *value) {
    drmModeObjectProperties *props = drmModeObjectGetProperties(
        fd, object_id, object_type);
    if (!props) {
        return -1;
    }

    int found = -1;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
        if (prop) {
            if (strcmp(prop->name, name) == 0) {
                *value = props->prop_values[i];
                found = 0;
            }
            drmModeFreeProperty(prop);
            if (found == 0) {
                break;
            }
        }
    }

    drmModeFreeObjectProperties(props);
    return found;
}

/* ========================================================================= */
/* Cursor plane discovery                                                    */
/* ========================================================================= */

// Find the cursor plane for the target CRTC
static uint32_t find_cursor_plane(drmtap_ctx *ctx) {
    drmModePlaneRes *planes = drmModeGetPlaneResources(ctx->drm_fd);
    if (!planes) {
        return 0;
    }

    uint32_t target_crtc = ctx->crtc_id;
    uint32_t result = 0;

    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *plane = drmModeGetPlane(ctx->drm_fd, planes->planes[i]);
        if (!plane) {
            continue;
        }

        /* Select the cursor plane by its CURRENT binding, not merely by the
         * possible_crtcs capability: on multi-monitor several cursor planes can
         * drive the same CRTC index, but only one is actually bound to our
         * target CRTC. Match plane->crtc_id == target_crtc (target_crtc == 0
         * means "the first currently-bound cursor plane"). This mirrors the
         * helper and avoids returning another monitor's cursor. */
        int crtc_ok = (target_crtc != 0) ? (plane->crtc_id == target_crtc)
                                         : (plane->crtc_id != 0);
        if (!crtc_ok) {
            drmModeFreePlane(plane);
            continue;
        }

        /* Check plane type = CURSOR */
        uint64_t type_val = 0;
        if (get_property_value(ctx->drm_fd, plane->plane_id,
                               DRM_MODE_OBJECT_PLANE, "type",
                               &type_val) == 0) {
            if (type_val == DRM_PLANE_TYPE_CURSOR) {
                result = plane->plane_id;
                drmModeFreePlane(plane);
                break;
            }
        }

        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(planes);
    return result;
}

/* ========================================================================= */
/* Public API                                                                */
/* ========================================================================= */

int drmtap_get_cursor(drmtap_ctx *ctx, drmtap_cursor_info *cursor) {
    if (!ctx || !cursor) {
        return -EINVAL;
    }

    memset(cursor, 0, sizeof(*cursor));

    /* When running unprivileged via the helper, the cursor plane's framebuffer
     * can only be read with CAP_SYS_ADMIN. Delegate the whole capture to the
     * helper (it locates the cursor plane for ctx->crtc_id and reads the image).
     * A live helper_fd means we are in that unprivileged-with-helper mode. */
    if (ctx->helper_fd >= 0) {
        return drmtap_helper_get_cursor(ctx, cursor);
    }

    uint32_t cursor_plane_id = find_cursor_plane(ctx);
    if (cursor_plane_id == 0) {
        /* No cursor plane is currently bound to this CRTC. Since the plane is
         * selected by its live binding, a hidden hardware cursor (which clears
         * the plane's CRTC_ID) lands here — treat it as hidden, exactly like the
         * helper's cursor_and_send does, not as an error. Returning -ENOENT
         * would make the consumer keep displaying a stale cursor. */
        cursor->visible = 0;
        return 0;
    }

    /* Get plane state */
    drmModePlane *plane = drmModeGetPlane(ctx->drm_fd, cursor_plane_id);
    if (!plane) {
        return -ENODEV;
    }

    if (plane->fb_id == 0) {
        /* Cursor is hidden */
        cursor->visible = 0;
        cursor->pixels = NULL;
        drmModeFreePlane(plane);
        return 0;
    }

    cursor->visible = 1;

    /* Read cursor position from plane properties */
    uint64_t crtc_x = 0, crtc_y = 0;
    get_property_value(ctx->drm_fd, cursor_plane_id,
                       DRM_MODE_OBJECT_PLANE, "CRTC_X", &crtc_x);
    get_property_value(ctx->drm_fd, cursor_plane_id,
                       DRM_MODE_OBJECT_PLANE, "CRTC_Y", &crtc_y);
    cursor->x = (int32_t)crtc_x;
    cursor->y = (int32_t)crtc_y;

    /* Read hotspot if available (VM drivers like virtio export this) */
    uint64_t hot_x = 0, hot_y = 0;
    get_property_value(ctx->drm_fd, cursor_plane_id,
                       DRM_MODE_OBJECT_PLANE, "HOTSPOT_X", &hot_x);
    get_property_value(ctx->drm_fd, cursor_plane_id,
                       DRM_MODE_OBJECT_PLANE, "HOTSPOT_Y", &hot_y);
    cursor->hot_x = (int32_t)hot_x;
    cursor->hot_y = (int32_t)hot_y;

    /* Get cursor framebuffer info */
    drmModeFB2 *fb2 = drmModeGetFB2(ctx->drm_fd, plane->fb_id);
    if (!fb2) {
        /* No direct privilege to read the cursor framebuffer — go via helper. */
        drmModeFreePlane(plane);
        return drmtap_helper_get_cursor(ctx, cursor);
    }

    cursor->width = fb2->width;
    cursor->height = fb2->height;

    /* Try to mmap cursor pixels if we have a handle */
    if (fb2->handles[0] != 0) {
        int prime_fd = -1;
        int ret = drmPrimeHandleToFD(ctx->drm_fd, fb2->handles[0],
                                     O_RDONLY | O_CLOEXEC, &prime_fd);
        if (ret == 0 && prime_fd >= 0) {
            uint32_t stride = fb2->pitches[0];
            /* A hardware cursor is tiny (e.g. 64x64 or 256x256 ARGB). Cap the
             * geometry so we never mmap/alloc an absurd size, and honor the
             * source stride (pitches[0]) instead of assuming stride == width*4
             * — a padded cursor fb would otherwise come out sheared. The width
             * cap is checked before width*4 so that product cannot overflow. */
            if (fb2->width <= 256 && fb2->height <= 256 &&
                stride != 0 && stride >= fb2->width * 4) {
                size_t map_size = (size_t)stride * fb2->height;
                void *mapped = mmap(NULL, map_size, PROT_READ, MAP_SHARED,
                                    prime_fd, 0);
                if (mapped != MAP_FAILED) {
                    /* Bracket the CPU read with a DMA-BUF sync, exactly as the
                     * main frame path does. DMA-BUF CPU access is not guaranteed
                     * coherent, so a non-coherent exporter (ARM / Tegra / Jetson)
                     * can otherwise hand back stale or partially-updated cursor
                     * pixels -- which the caller's content hash then suppresses,
                     * freezing the remote cursor. */
                    struct dma_buf_sync sync = {
                        .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ
                    };
                    drmIoctl(prime_fd, DMA_BUF_IOCTL_SYNC, &sync);
                    size_t tight = (size_t)fb2->width * fb2->height * 4;
                    cursor->pixels = malloc(tight);
                    if (cursor->pixels) {
                        /* Repack into tightly-packed width*4 rows (the API
                         * contract) honoring the source stride. */
                        for (uint32_t y = 0; y < fb2->height; y++) {
                            memcpy((uint8_t *)cursor->pixels +
                                       (size_t)y * fb2->width * 4,
                                   (const uint8_t *)mapped + (size_t)y * stride,
                                   (size_t)fb2->width * 4);
                        }
                    }
                    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
                    drmIoctl(prime_fd, DMA_BUF_IOCTL_SYNC, &sync);
                    munmap(mapped, map_size);
                }
            }
            close(prime_fd);
        }
    }

    /* drmModeGetFB2 minted a fresh GEM handle we own; close it before freeing fb2,
     * or the privileged direct cursor path leaks a kernel handle (pinning the BO)
     * on every poll. The pixels were already copied out and the prime_fd closed. */
    if (fb2->handles[0] != 0) {
        struct drm_gem_close gc = { .handle = fb2->handles[0] };
        drmIoctl(ctx->drm_fd, DRM_IOCTL_GEM_CLOSE, &gc);
    }
    drmModeFreeFB2(fb2);
    drmModeFreePlane(plane);

    /* If the framebuffer existed but its handle wasn't readable (handles[0]==0,
     * i.e. we lack CAP_SYS_ADMIN), we have a visible cursor but no pixels — read
     * it through the privileged helper instead. */
    if (cursor->visible && cursor->pixels == NULL) {
        return drmtap_helper_get_cursor(ctx, cursor);
    }

    drmtap_debug_log(ctx, "cursor: %ux%u at (%d,%d) hotspot=(%d,%d) %s",
                     cursor->width, cursor->height,
                     cursor->x, cursor->y,
                     cursor->hot_x, cursor->hot_y,
                     cursor->visible ? "visible" : "hidden");

    return 0;
}

void drmtap_cursor_release(drmtap_ctx *ctx, drmtap_cursor_info *cursor) {
    (void)ctx;
    if (!cursor) {
        return;
    }
    free(cursor->pixels);
    cursor->pixels = NULL;
}
