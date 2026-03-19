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

    /* Get CRTC index */
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

    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *plane = drmModeGetPlane(ctx->drm_fd, planes->planes[i]);
        if (!plane) {
            continue;
        }

        /* Check if plane can drive our CRTC */
        if (!(plane->possible_crtcs & (1u << crtc_index))) {
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

    uint32_t cursor_plane_id = find_cursor_plane(ctx);
    if (cursor_plane_id == 0) {
        return -ENOENT;  /* No cursor plane */
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
        drmModeFreePlane(plane);
        return -EACCES;  /* Likely needs CAP_SYS_ADMIN */
    }

    cursor->width = fb2->width;
    cursor->height = fb2->height;

    /* Try to mmap cursor pixels if we have a handle */
    if (fb2->handles[0] != 0) {
        int prime_fd = -1;
        int ret = drmPrimeHandleToFD(ctx->drm_fd, fb2->handles[0],
                                     O_RDONLY | O_CLOEXEC, &prime_fd);
        if (ret == 0 && prime_fd >= 0) {
            size_t size = (size_t)fb2->width * fb2->height * 4;
            void *mapped = mmap(NULL, size, PROT_READ, MAP_SHARED,
                                prime_fd, 0);
            if (mapped != MAP_FAILED) {
                /* Copy cursor pixels (cursor is small, ~64×64) */
                cursor->pixels = malloc(size);
                if (cursor->pixels) {
                    memcpy(cursor->pixels, mapped, size);
                }
                munmap(mapped, size);
            }
            close(prime_fd);
        }
    }

    drmModeFreeFB2(fb2);
    drmModeFreePlane(plane);

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
