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
 *   - Cursor position (CRTC_X, CRTC_Y properties of the cursor plane: where the
 *     image is placed on the CRTC, not the click point)
 *   - Cursor image (framebuffer attached to the cursor plane)
 *   - Hotspot (HOTSPOT_X, HOTSPOT_Y properties, para-virtualized drivers only —
 *     absent on i915/amdgpu/nvidia, where these read back 0)
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
/* wire_hot_measured(): the hotspot-completeness rule, shared with the helper so
 * the direct read and the helper read cannot disagree. */
#include "wire.h"

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

/* See drmtap_internal.h. The helper is only worth a fork/exec when the direct
 * read failed for lack of privilege: a GETFB2 the kernel refused with EACCES or
 * EPERM, or one that succeeded but returned no GEM handle (the result an
 * unprivileged caller gets). Every other case is transient or a resource limit
 * the helper cannot improve: a retired fb_id (a cursor shape-change race, usually
 * ENOENT), or a handle we could not turn into pixels. Those the next poll clears,
 * and the consumer keeps the current shape meanwhile. */
int drmtap_cursor_needs_helper(int fb2_ok, int err, int had_handle) {
    if (!fb2_ok) {
        return err == EACCES || err == EPERM;
    }
    return had_handle == 0;
}

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
        /* No plane, so no properties to have read: the zero hotspot is an
         * absence, and saying so is more useful to a consumer than -ENOTSUP. */
        drmtap_cursor_set_hot_provenance(cursor, 0);
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
        drmtap_cursor_set_hot_provenance(cursor, 0);
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

    /* Read hotspot if available (VM drivers like virtio export this).
     *
     * Both return values are KEPT here. They used to be discarded, which threw
     * away the only fact that separates "this driver publishes no hotspot" from
     * "this driver publishes a hotspot and it is (0, 0)": the coordinates read
     * the same in both cases. A consumer that corrects a rotated cursor has to
     * know which one it is holding, so the answer is recorded on the sample and
     * read back with drmtap_cursor_hotspot_valid(). Measured means BOTH were
     * found: a plane exposing one of the pair would otherwise contribute a real
     * coordinate and an invented zero. */
    uint64_t hot_x = 0, hot_y = 0;
    int have_hot_x = get_property_value(ctx->drm_fd, cursor_plane_id,
                       DRM_MODE_OBJECT_PLANE, "HOTSPOT_X", &hot_x) == 0;
    int have_hot_y = get_property_value(ctx->drm_fd, cursor_plane_id,
                       DRM_MODE_OBJECT_PLANE, "HOTSPOT_Y", &hot_y) == 0;
    cursor->hot_x = (int32_t)hot_x;
    cursor->hot_y = (int32_t)hot_y;
    /* Set before any later exit, so every path out of this function that got
     * this far carries the answer — including the transient-miss returns. */
    drmtap_cursor_set_hot_provenance(cursor,
                                     wire_hot_measured(have_hot_x, have_hot_y));

    /* Get cursor framebuffer info */
    drmModeFB2 *fb2 = drmModeGetFB2(ctx->drm_fd, plane->fb_id);
    if (!fb2) {
        /* Two very different failures land here. A kernel that refuses GETFB2 to
         * a non-master caller signals a lack of privilege (EACCES/EPERM), which
         * is the helper's job. Any other errno — typically ENOENT — means the
         * fb_id was retired between the plane read above and this call, a race
         * with a cursor shape change that the helper cannot fix. Fork only for
         * the privilege case; treat the race as a transient miss. */
        int err = errno;
        uint32_t fb_id = plane->fb_id;
        drmModeFreePlane(plane);
        if (drmtap_cursor_needs_helper(0, err, 0)) {
            return drmtap_helper_get_cursor(ctx, cursor);
        }
        drmtap_debug_log(ctx,
            "cursor: fb %u unreadable (%s); transient, keeping the current shape",
            fb_id, strerror(err));
        cursor->pixels = NULL;
        return 0;
    }

    cursor->width = fb2->width;
    cursor->height = fb2->height;

    /* GETFB2 hands back a GEM handle only to a privileged caller; its absence is
     * the signal we lack that privilege. Captured before fb2 is freed, so the
     * final decision below can tell "no privilege" from "had a handle but could
     * not read it". */
    int had_handle = (fb2->handles[0] != 0);

    /* Try to mmap cursor pixels if we have a handle */
    if (had_handle) {
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
                    /* Track whether START actually took, and only issue the
                     * matching END if it did -- an unpaired END would unbalance
                     * the exporter's CPU-access accounting. A failure is logged
                     * and the read still proceeds: it degrades to the previous
                     * (unsynchronised) behaviour rather than dropping the cursor
                     * on an exporter that does not implement the ioctl, and a
                     * stale cursor is cosmetic, not corruption. */
                    int sync_started =
                        drmIoctl(prime_fd, DMA_BUF_IOCTL_SYNC, &sync) == 0;
                    if (!sync_started) {
                        drmtap_debug_log(ctx,
                            "cursor: DMA_BUF_SYNC_START failed (%s); reading "
                            "without cache invalidation", strerror(errno));
                    }
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
                    if (sync_started) {
                        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
                        if (drmIoctl(prime_fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
                            drmtap_debug_log(ctx,
                                "cursor: DMA_BUF_SYNC_END failed (%s)",
                                strerror(errno));
                        }
                    }
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

    /* A visible cursor with no pixels has two causes and only one wants the
     * helper. No GEM handle means GETFB2 gave us the fb but not the privilege to
     * read it (no CAP_SYS_ADMIN / not DRM master): the helper's job. Having had a
     * handle but still no pixels — the prime export, the mmap, the size cap or the
     * alloc — is transient or a resource limit the helper cannot improve, so skip
     * this poll and let the consumer keep the current shape. */
    if (cursor->visible && cursor->pixels == NULL) {
        if (drmtap_cursor_needs_helper(1, 0, had_handle)) {
            return drmtap_helper_get_cursor(ctx, cursor);
        }
        drmtap_debug_log(ctx,
            "cursor: fb readable but no pixels this poll; transient, keeping shape");
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
    /* The provenance describes the sample that just went away. Leaving it set
     * would let a released struct keep answering about pixels it no longer has,
     * and a reused one answer about the previous sample if the next fill fails
     * before recording its own. */
    cursor->_priv = NULL;
}

int drmtap_cursor_hotspot_valid(const drmtap_cursor_info *cursor, int *valid) {
    if (!cursor || !valid) {
        return -EINVAL;
    }
    uintptr_t flags = (uintptr_t)cursor->_priv;
    if (!(flags & CURSOR_PRIV_HOT_ANSWERED)) {
        /* Nothing recorded the provenance for this sample: a struct that was
         * never filled, one already released, or one filled through a path that
         * predates this entry point (an older privileged helper, whose reply
         * carried no such field). Refusing is the only honest answer — reporting
         * 0 would claim the hotspot was a guess, which is a claim nobody made. */
        return -ENOTSUP;
    }
    *valid = (flags & CURSOR_PRIV_HOT_MEASURED) ? 1 : 0;
    return 0;
}
