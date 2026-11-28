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
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
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
#include "layers_kms.h"
#include <drm_fourcc.h>

_Static_assert(DRMTAP_PACKET_MAX_BYTES >= DRMTAP_PKT_MAX_LEN,
               "a full packet must fit drmtap_packet.bytes");
_Static_assert(DRMTAP_PACKET_MAX_FDS == DRMTAP_LAYERS_MAX_FDS,
               "drmtap_packet.fds and the layer set carry the same fds");
_Static_assert(sizeof(drmtap_packet) == DRMTAP_PACKET_MAX_BYTES + 8 + DRMTAP_PACKET_MAX_FDS * sizeof(int),
               "drmtap_packet is caller-owned and frozen: never grow it");

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

// GetFB2 mints a handle per plane, fresh on every call; only handles[0] is used here,
// so the rest leak one per grab when the planes live in separate BOs (CCS). Dedupe
// before closing: planes sharing a BO return the SAME handle, and a second close
// would free one the caller still owns.
static void close_auxiliary_gem_handles(drmtap_ctx *ctx, const drmModeFB2 *fb2) {
    for (int p = 1; p < 4; p++) {
        uint32_t h = fb2->handles[p];
        if (h == 0 || h == fb2->handles[0]) {
            continue;
        }
        int dup = 0;
        for (int q = 1; q < p; q++) {
            if (fb2->handles[q] == h) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            drmtap_gem_close(ctx, h);
        }
    }
}

/* Test hook: DRMTAP_FORCE_MMAP_FAIL=1 makes the fast-path cache-miss drop a
 * successful CPU mapping so the EGL-detile fd fallback runs on any EGL-capable GPU,
 * not only a discrete/tiled one that genuinely refuses the mmap. Off by default. */
static int drmtap_force_mmap_fail(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = secure_getenv("DRMTAP_FORCE_MMAP_FAIL");
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

/* A context opened with crtc_id 0 captures the first CRTC that has a mode. It is chosen
 * once and kept in ctx->crtc_id, so every later call answers for the same CRTC. */
static uint32_t select_first_active_crtc(drmtap_ctx *ctx) {
    drmModeRes *res = drmModeGetResources(ctx->drm_fd);
    if (!res) {
        return 0;
    }
    uint32_t chosen = 0;
    for (int i = 0; i < res->count_crtcs && chosen == 0; i++) {
        drmModeCrtc *crtc = drmModeGetCrtc(ctx->drm_fd, res->crtcs[i]);
        if (!crtc) {
            continue;
        }
        if (crtc->mode_valid) {
            chosen = crtc->crtc_id;
        }
        drmModeFreeCrtc(crtc);
    }
    drmModeFreeResources(res);
    if (chosen != 0) {
        ctx->crtc_id = chosen;
        drmtap_debug_log(ctx, "auto-selected CRTC %u", chosen);
    }
    return chosen;
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
        target_crtc = select_first_active_crtc(ctx);
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
    return result;
}

/* The exact refresh of the captured CRTC, from its current mode. No connector probe:
 * once the CRTC is known it is one GETCRTC, so a caller can ask again to follow a
 * mode change. */
static int drmtap_crtc_refresh_impl(drmtap_ctx *ctx, uint64_t *num, uint64_t *den);
/* Public entry: see drmtap_error_ensure (every error return leaves a message). */
int drmtap_crtc_refresh(drmtap_ctx *ctx, uint64_t *num, uint64_t *den) {
    unsigned mark = drmtap_error_mark(ctx);
    return drmtap_error_ensure(ctx, mark, drmtap_crtc_refresh_impl(ctx, num, den), "drmtap_crtc_refresh");
}
static int drmtap_crtc_refresh_impl(drmtap_ctx *ctx, uint64_t *num, uint64_t *den) {
    if (!ctx || !num || !den) {
        return -EINVAL;
    }
    if (ctx->is_render_only) {
        drmtap_set_error(ctx, "crtc refresh: a render-only context has no CRTC");
        return -ENOTSUP;
    }
    uint32_t crtc_id = ctx->crtc_id ? ctx->crtc_id : select_first_active_crtc(ctx);
    if (crtc_id == 0) {
        drmtap_set_error(ctx, "crtc refresh: no CRTC with a mode to pick");
        return -ENOENT;
    }
    drmModeCrtc *crtc = drmModeGetCrtc(ctx->drm_fd, crtc_id);
    if (!crtc) {
        int err = errno ? errno : EIO;
        drmtap_set_error(ctx, "crtc %u: %s", crtc_id, strerror(err));
        return -err;
    }
    int rc = crtc->mode_valid ? drmtap_mode_refresh(&crtc->mode, num, den) : -ENODATA;
    drmModeFreeCrtc(crtc);
    if (rc != 0) {
        drmtap_set_error(ctx, "crtc %u has no mode with timings", crtc_id);
        return -ENODATA;
    }
    return 0;
}

/* The DRM "rotation" property of the plane the last grab read from. Read now,
 * so a consumer calls it right after the grab it wants to describe. The plane is
 * the one do_grab or the fast path recorded (before any grab: the one a grab would
 * use), never a fresh sweep, so the answer is for the same plane the frame came
 * from. The property id is looked up by name once per plane and reused. */
static int drmtap_plane_rotation_impl(drmtap_ctx *ctx, uint32_t *rotation);
/* Public entry: see drmtap_error_ensure (every error return leaves a message). */
int drmtap_plane_rotation(drmtap_ctx *ctx, uint32_t *rotation) {
    unsigned mark = drmtap_error_mark(ctx);
    return drmtap_error_ensure(ctx, mark, drmtap_plane_rotation_impl(ctx, rotation), "drmtap_plane_rotation");
}
static int drmtap_plane_rotation_impl(drmtap_ctx *ctx, uint32_t *rotation) {
    if (!ctx || !rotation) {
        return -EINVAL;
    }
    /* A composed frame is the CRTC raster: every plane was drawn with its own
     * rotation applied, so it is in screen orientation, like a framebuffer the
     * compositor rotated in software. Answer ROTATE_0 for it, whichever plane it
     * came from and on a render-only context too (convert_packet). */
    if (ctx->last_frame_composed == DRMTAP_FRAME_ONE_LAYER) {
        /* convert_packet converted one layer as its buffer, unrotated: the answer is
         * that plane's rotation, which the packet carries (also on a render-only
         * context, and where the exporter assumed a composed frame). */
        *rotation = ctx->packet_rotation;
        return 0;
    }
    if (ctx->last_frame_composed) {
        *rotation = DRMTAP_ROT_0;
        return 0;
    }
    if (ctx->is_render_only) {
        return -ENOTSUP;
    }
    uint32_t plane_id = ctx->grab_plane_id ? ctx->grab_plane_id : find_primary_plane(ctx);
    if (plane_id == 0) {
        return -ENOENT;
    }
    if (plane_id != ctx->rot_prop_plane_id) {
        /* A different plane may have a different property set: look it up again. */
        ctx->rot_prop_plane_id = plane_id;
        ctx->rot_prop_id = 0;
        ctx->rot_prop_state = 0;
    }
    if (ctx->rot_prop_state < 0) {
        return -ENOTSUP;
    }
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(ctx->drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props) {
        int err = errno ? errno : EIO;
        drmtap_set_error(ctx, "plane %u properties: %s", plane_id, strerror(err));
        return -err;
    }
    int found = 0;
    int read_failed = 0;
    uint64_t value = 0;
    if (ctx->rot_prop_state == 0) {
        /* First look at this plane: find the property by name and remember its id. */
        for (uint32_t i = 0; i < props->count_props && !found; i++) {
            drmModePropertyRes *p = drmModeGetProperty(ctx->drm_fd, props->props[i]);
            if (!p) {
                /* Unreadable, so "absent" cannot be concluded: keep the lookup pending. */
                read_failed = errno ? errno : EIO;
                continue;
            }
            if (strcmp(p->name, "rotation") == 0) {
                ctx->rot_prop_id = props->props[i];
                value = props->prop_values[i];
                found = 1;
            }
            drmModeFreeProperty(p);
        }
        if (found) {
            ctx->rot_prop_state = 1;
        } else if (!read_failed) {
            ctx->rot_prop_state = -1;
        }
    } else {
        for (uint32_t i = 0; i < props->count_props; i++) {
            if (props->props[i] == ctx->rot_prop_id) {
                value = props->prop_values[i];
                found = 1;
                break;
            }
        }
        if (!found) {
            /* The property set changed under us: look it up by name next time. */
            ctx->rot_prop_state = 0;
            ctx->rot_prop_id = 0;
        }
    }
    drmModeFreeObjectProperties(props);
    if (found) {
        *rotation = (uint32_t)value;
        return 0;
    }
    if (read_failed) {
        drmtap_set_error(ctx, "plane %u property read: %s", plane_id, strerror(read_failed));
        return -read_failed;
    }
    return -ENOTSUP;
}

/* The connector HDR metadata for this CRTC, so the conversion path can tone-map.
 * Used to live at the end of find_primary_plane, which made a plane LOOKUP rewrite
 * ctx->cur_hdr_eotf as a side effect. That was invisible until the scanout-width
 * decision started looking up the plane too: a pure geometry query then clobbered
 * the HDR state, and on the helper path (where these values arrive over the wire,
 * see drmtap_helper_grab) only the order of two assignments kept the wire values
 * from being replaced by a direct read that an unprivileged process cannot even
 * make. Called explicitly by the paths that want it now. */
static void read_hdr_for_current_crtc(drmtap_ctx *ctx) {
    read_hdr_metadata_direct(ctx, ctx->crtc_id);
}

/* The lit non-cursor planes of this context's CRTC (layers_kms.h), picking the
 * CRTC first when the context has none yet, the way find_primary_plane does.
 * Returns the lit count, 0 when nothing scans out, -ENOTSUP when the plane
 * properties are hidden (no atomic cap: the caller keeps the legacy path), or
 * another -errno. Costs one OBJ_GETPROPERTIES per plane that can drive the CRTC. */
static int read_layers(drmtap_ctx *ctx, drmtap_layer_set *set) {
    if (ctx->crtc_id == 0 && select_first_active_crtc(ctx) == 0) {
        memset(set, 0, sizeof(*set));
        return 0;
    }
    int n = drmtap_kms_read_layers(ctx->drm_fd, &ctx->layer_cache, ctx->crtc_id, set);
    if (n >= 0) {
        if ((uint32_t)n != ctx->last_layer_count) {
            drmtap_debug_log(ctx, "crtc %u: %d lit plane%s", ctx->crtc_id, n,
                             n == 1 ? "" : "s");
        }
        ctx->last_layer_count = (uint32_t)n;
    }
    return n;
}

/* ========================================================================= */
/* DMA-BUF sync helpers                                                      */
/* ========================================================================= */

static int dmabuf_sync_start(int fd) {
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ
    };
    return drmIoctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

static int dmabuf_sync_end(int fd) {
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ
    };
    return drmIoctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

// Close whatever CPU-access window the fast path left open.
//
// The slow path pairs its START with an END in drmtap_frame_release, but a fast
// frame points into a mapping the context keeps CACHED across frames and carries
// no _priv, so it is never released and there is no such moment. The window
// therefore closes at the next grab, which is also when the caller has provably
// finished reading the previous frame. Called before every new START, before a
// slot can be evicted, and from drmtap_fast_cleanup, so a slot owes at most one
// END at any time.
//
// At most one slot can be open, but sweeping all of them is what makes that an
// invariant rather than an assumption.
static void fast_sync_close(drmtap_ctx *ctx) {
    for (int i = 0; i < DRMTAP_FAST_SLOTS; i++) {
        if (ctx->fast_slots[i].sync_started && ctx->fast_slots[i].prime_fd >= 0) {
            dmabuf_sync_end(ctx->fast_slots[i].prime_fd);
        }
        ctx->fast_slots[i].sync_started = 0;
    }
}

// Invalidate the CPU caches for a cached slot before reading it, recording the
// START so exactly one END follows. The virtio sub-path does NOT come through
// here: it makes the buffer coherent with TRANSFER_FROM_HOST instead of a
// dma-buf sync, so pairing an END with it would be an END that never had a
// START.
static void fast_sync_begin(drmtap_ctx *ctx, int slot) {
    fast_sync_close(ctx);
    if (ctx->fast_slots[slot].prime_fd >= 0) {
        ctx->fast_slots[slot].sync_started =
            (dmabuf_sync_start(ctx->fast_slots[slot].prime_fd) == 0);
    }
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

/* Defined further down with the convert path that first needed it. Declared here
 * because the helper wire has to apply the same stride-covers-width bound. */
static uint32_t format_min_bpp(uint32_t fourcc);

int drmtap_validate_fb_dims(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return -EINVAL;
    }
    /* Bound each dimension BEFORE any multiply, so the product below cannot wrap
     * (even where size_t is 32-bit): 32768*32768 == 2^30, and *4 == 2^32. -EFBIG,
     * not -EINVAL: an over-range dimension is the too-large case, and callers
     * (and tests) already distinguish "oversized geometry" from "malformed". */
    if (width > DRMTAP_MAX_DIM || height > DRMTAP_MAX_DIM) {
        return -EFBIG;
    }
    if ((size_t)width * height > DRMTAP_MAX_FB_BYTES / 4) {
        return -EFBIG;
    }
    return 0;
}

int drmtap_ensure_out(drmtap_ctx *ctx, size_t size, void **out) {
    if (size == 0) {
        return -EINVAL;
    }
    ctx->out_gen++;
    /* Cap the frame itself even when the destination is the caller's: a caller
     * that happens to own a huge mapping must not be able to unlock a frame
     * larger than this library is willing to handle, and keeping the cap on both
     * paths means the caller-supplied case is not the loose one. */
    if (size > DRMTAP_MAX_FB_BYTES) {
        return -EFBIG;
    }
    if (ctx->user_out) {
        if (size > ctx->user_out_len) {
            /* Refuse rather than write short. The caller sized its buffer for a
             * geometry; if the display changed under it, a partial frame would be
             * indistinguishable from a good one, and the caller cannot re-mmap what
             * it does not know went stale. Named through drmtap_error so this is
             * actionable and not just an errno. */
            drmtap_set_error(ctx,
                "output buffer is %zu bytes but this frame needs %zu "
                "(geometry changed?); call drmtap_set_output_buffer again with a "
                "buffer of at least that size, or pass NULL to go back to the "
                "library-owned buffer",
                ctx->user_out_len, size);
            return -ENOSPC;
        }
        *out = ctx->user_out;
        return 0;
    }
    int ret = drmtap_ensure_buf(&ctx->deswizzle_buf, &ctx->deswizzle_buf_size, size);
    if (ret != 0) {
        return ret;
    }
    *out = ctx->deswizzle_buf;
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
        int err = errno ? errno : EIO;
        drmtap_set_error(ctx, "virtio: TRANSFER_FROM_HOST failed: %s%s", strerror(err),
                         err == ENOSYS ? " (virtio-gpu without 3D/virgl)" : "");
        return -err;
    }

    /* Wait for transfer completion */
    struct drm_virtgpu_3d_wait wait_args = {0};
    wait_args.handle = handle;
    ret = drmIoctl(ctx->drm_fd, DRM_IOCTL_VIRTGPU_WAIT, &wait_args);
    if (ret != 0) {
        int err = errno ? errno : EIO;
        drmtap_set_error(ctx, "virtio: VIRTGPU_WAIT failed: %s", strerror(err));
        return -err;
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

/* fb2->modifier is only meaningful when the framebuffer was created with the
 * DRM_MODE_FB_MODIFIERS flag; with the flag clear the field is undefined (commonly
 * 0, garbage on some drivers). Trusting a bogus LINEAR on a driver that is actually
 * tiling corrupts the import, so report INVALID and let the layout be inferred.
 * Open-coded identically at every frame-filling site; named here because the
 * scanout-width decision needs the same answer. */
static uint64_t fb2_effective_modifier(const drmModeFB2 *fb2) {
    return (fb2->flags & DRM_MODE_FB_MODIFIERS) ? fb2->modifier
                                                : DRM_FORMAT_MOD_INVALID;
}

/* Report the width the CRTC actually scans out, not the width of the framebuffer
 * OBJECT. A driver may allocate the scanout fb wider than the mode to satisfy a
 * pitch alignment: Apple's appletbdrm (the Touch Bar strip) drives a 60x2170 mode
 * from a 64x2170 fb with a 256-byte pitch, and those four columns are padding that
 * is never scanned out. Reporting the fb width hands the caller an image wider
 * than the display it asked to capture. In rustdesk that read as "this display
 * never matched its advertised geometry" -- the display list carries the mode --
 * so every frame was rejected, the display was demoted to PipeWire, and the
 * client sat on "waiting for image".
 *
 * This only ever SHRINKS, and only the width:
 *  - An fb NARROWER than the mode means the CRTC is scaling a smaller buffer up
 *    to its mode. That is a genuinely different image, not padding, so it stays
 *    reported as it is and the caller can decide.
 *  - A CRTC whose viewport does not start at the fb origin (crtc->x/y != 0, i.e.
 *    several heads scanning out of one big framebuffer) needs an OFFSET crop too.
 *    drmtap_dmabuf_desc has a frozen layout with no crop origin, so such a frame
 *    is left whole; untested here, hence the one-time log rather than a guess.
 *
 * Buffer-size arithmetic must keep using fb2->width and fb2->pitches: the
 * allocation is still the full padded framebuffer. Only the REPORTED geometry
 * narrows, and the caller reads rows out of it at the unchanged stride.
 */
/* The decision alone, with no DRM fd, so it is unit-testable on any machine: the
 * padded-scanout case needs hardware that pads (Apple's Touch Bar) to observe, and a
 * rule this easy to get subtly wrong should not be reachable only through that one
 * laptop. Reports through *why which branch was taken.
 *
 * Only ever SHRINKS, and only the width. Every branch that declines to narrow is a
 * case where the framebuffer width IS the right answer:
 *
 *  - TILED. The CPU deswizzle derives its tile grid from the width and uses it to
 *    address the SOURCE (deswizzle_nvidia_x_tiled: tiles_x = ceil(width/tile_w), then
 *    src_off = (tile_row * tiles_x + tx) * tile_size; it ignores src_stride). A width
 *    short by a tile or more mis-addresses every tile row after the first and silently
 *    mangles the image. The visible width and the width the tiling was laid out for
 *    are different quantities. No padded tiled scanout has been observed here, so it
 *    is left alone rather than narrowed on a guess.
 *  - SCALING. A plane whose SRC rect is larger than its CRTC rect is genuinely
 *    downscaling: the whole framebuffer IS being displayed, just smaller. Narrowing
 *    would hand the caller the left part of the image and call it the whole screen.
 *    A framebuffer wider than the mode looks IDENTICAL to pitch padding until the
 *    plane rect is read, which is the entire reason this needs the plane and cannot
 *    be decided from the mode alone. Note a pitch test cannot substitute:
 *    fb_width * bpp == pitches[0] holds for a 3840-wide downscaled framebuffer just
 *    as it does for the padded 64-wide one.
 *  - OFFSET. The plane reads from a non-zero SRC_X/SRC_Y, i.e. several heads out of
 *    one big framebuffer. That needs an offset crop, and drmtap_dmabuf_desc has a
 *    frozen layout with nowhere to carry a crop origin. No hardware here has it.
 *  - UNREADABLE. Without the plane rect the padding and the downscale are
 *    indistinguishable, so the safe answer is the pre-existing behaviour.
 */
uint32_t drmtap_scanout_width_of(uint32_t fb_width,
                                 const drmtap_plane_rect *rect,
                                 int layout_is_linear,
                                 drmtap_scanout_why *why) {
    drmtap_scanout_why w = DRMTAP_SCANOUT_AS_IS;
    uint32_t out = fb_width;

    if (!layout_is_linear) {
        if (rect && rect->valid && rect->src_w > 0 && rect->src_w < fb_width) {
            w = DRMTAP_SCANOUT_TILED_NOT_NARROWED;
        }
    } else if (!rect || !rect->valid) {
        /* Only worth reporting when there was something to decide. */
        w = (fb_width > 0) ? DRMTAP_SCANOUT_NO_PLANE_RECT : DRMTAP_SCANOUT_AS_IS;
    } else if (rect->src_x != 0 || rect->src_y != 0) {
        w = DRMTAP_SCANOUT_OFFSET_UNSUPPORTED;
    } else if (rect->src_w != rect->crtc_w || rect->src_h != rect->crtc_h) {
        w = DRMTAP_SCANOUT_SCALING_NOT_NARROWED;
    } else if (rect->src_w > 0 && rect->src_w < fb_width) {
        out = rect->src_w;
        w = DRMTAP_SCANOUT_NARROWED;
    }

    if (why) {
        *why = w;
    }
    return out;
}

/* Read the primary plane rectangle of the context's CRTC. The SRC and CRTC properties
 * are atomic-only: a client that has not asked for DRM_CLIENT_CAP_ATOMIC is shown
 * none of them (measured on appletbdrm -- SRC_W present with the cap, absent without).
 * The cap is set for every context in drmtap_open() (connector CRTC_ID needs it too),
 * and requested again here so this path keeps working if that ever stops being true.
 * It is a per-fd flag, needs no privilege and no DRM master, no atomic commit is ever
 * issued, and no other client is affected. rect->valid stays 0 when
 * anything is missing, which the decision above treats as "cannot tell". */
static void read_primary_plane_rect(drmtap_ctx *ctx, drmtap_plane_rect *rect) {
    memset(rect, 0, sizeof(*rect));

    if (!ctx->atomic_cap_tried) {
        ctx->atomic_cap_tried = 1;
        if (drmSetClientCap(ctx->drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
            drmtap_debug_log(ctx,
                "DRM_CLIENT_CAP_ATOMIC refused (%s); plane SRC_W is unreadable, so a "
                "padded scanout will be reported at its framebuffer width",
                strerror(errno));
        }
    }

    uint32_t plane_id = find_primary_plane(ctx);
    if (plane_id == 0) {
        return;
    }
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(ctx->drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props) {
        return;
    }

    /* 16.16 fixed point for the SRC rect, plain pixels for the CRTC rect. Fractions are dropped:
     * a fractional source rect is a scaling plane, and the equality test below
     * already declines to narrow that. */
    int got = 0;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(ctx->drm_fd, props->props[i]);
        if (!p) {
            continue;
        }
        uint64_t v = props->prop_values[i];
        if      (!strcmp(p->name, "SRC_X"))  { rect->src_x  = (uint32_t)(v >> 16); got |= 1; }
        else if (!strcmp(p->name, "SRC_Y"))  { rect->src_y  = (uint32_t)(v >> 16); got |= 2; }
        else if (!strcmp(p->name, "SRC_W"))  { rect->src_w  = (uint32_t)(v >> 16); got |= 4; }
        else if (!strcmp(p->name, "SRC_H"))  { rect->src_h  = (uint32_t)(v >> 16); got |= 8; }
        else if (!strcmp(p->name, "CRTC_W")) { rect->crtc_w = (uint32_t)v;         got |= 16; }
        else if (!strcmp(p->name, "CRTC_H")) { rect->crtc_h = (uint32_t)v;         got |= 32; }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    rect->valid = (got == 63);
}

/* Report the width the CRTC actually scans out, not the width of the framebuffer
 * OBJECT. A driver may allocate the scanout fb wider than the mode to satisfy a pitch
 * alignment: Apple's appletbdrm (the Touch Bar strip) drives a 60x2170 mode from a
 * 64x2170 fb with a 256-byte pitch, and its primary plane reads SRC_W=60 -- those four
 * columns are padding that is never scanned out. Reporting the fb width hands the
 * caller an image wider than the display it asked to capture. In rustdesk that read as
 * "this display never matched its advertised geometry" -- the display list carries the
 * mode -- so every frame was rejected, the display was demoted to PipeWire, and the
 * client sat on "waiting for image".
 *
 * The mode is only the CHEAP GATE here: it says "worth looking closer", and the plane
 * rect decides. Buffer-size arithmetic must keep using fb2->width and fb2->pitches:
 * the allocation is still the full padded framebuffer. Only the REPORTED geometry
 * narrows, and the caller reads rows out of it at the unchanged stride.
 */
static uint32_t drmtap_scanout_width(drmtap_ctx *ctx, uint32_t fb_width,
                                     uint64_t modifier) {
    if (ctx->crtc_id == 0) {
        return fb_width;
    }
    drmModeCrtc *crtc = drmModeGetCrtc(ctx->drm_fd, ctx->crtc_id);
    if (!crtc) {
        return fb_width;
    }
    int worth_looking = crtc->mode_valid && crtc->mode.hdisplay > 0 &&
                        crtc->mode.hdisplay < fb_width;
    uint32_t hdisplay = crtc->mode.hdisplay;
    /* Both are copied out HERE because crtc is freed on the next line, so neither
     * declaration can move down to its use. cppcheck 2.19 asks for exactly that
     * for vdisplay, whose only use is the log line further down; taking that
     * advice would read freed memory. The CI image carries an older cppcheck that
     * does not report it, so suppress it rather than wait for the image to update
     * and break the build. */
    // cppcheck-suppress variableScope
    uint32_t vdisplay = crtc->mode.vdisplay;
    drmModeFreeCrtc(crtc);
    if (!worth_looking) {
        return fb_width;
    }

    /* Serve a memoized answer rather than re-reading the plane rect on every frame of
     * a padded scanout: the decision only changes when this key changes, and the read
     * costs a plane sweep plus a drmModeGetProperty per property. */
    if (!ctx->sw_override_on && ctx->sw_cached && ctx->sw_key_crtc == ctx->crtc_id &&
        ctx->sw_key_hdisplay == hdisplay && ctx->sw_key_fb_width == fb_width &&
        ctx->sw_key_modifier == modifier) {
        return ctx->sw_cached_width;
    }

    /* LINEAR and INVALID (unknown layout, handled downstream as linear) are the
     * layouts whose only width-dependent consumer is the OUTPUT: rows are addressed
     * through the stride, so narrowing the reported width is safe. Anything else is a
     * real tiling -- see drmtap_scanout_width_of. */
    int linear = (modifier == DRM_FORMAT_MOD_LINEAR ||
                  modifier == DRM_FORMAT_MOD_INVALID);

    drmtap_plane_rect rect;
    if (ctx->sw_override_on) {
        /* The frame comes from another plane than the primary (grab_desc's covering
         * layer): decide from that plane's rect, and keep it out of the memo. */
        rect = ctx->sw_override;
        return drmtap_scanout_width_of(fb_width, &rect, linear, NULL);
    }
    read_primary_plane_rect(ctx, &rect);

    drmtap_scanout_why why = DRMTAP_SCANOUT_AS_IS;
    uint32_t out = drmtap_scanout_width_of(fb_width, &rect, linear, &why);

    if (why != DRMTAP_SCANOUT_AS_IS && !ctx->logged_scanout_crop) {
        ctx->logged_scanout_crop = 1;
        switch (why) {
        case DRMTAP_SCANOUT_NARROWED:
            drmtap_debug_log(ctx,
                "crtc %u scans out %u of the %u-pixel-wide scanout fb (pitch padding; "
                "plane reads SRC %ux%u 1:1); reporting %u",
                ctx->crtc_id, rect.src_w, fb_width, rect.src_w, rect.src_h, out);
            break;
        case DRMTAP_SCANOUT_TILED_NOT_NARROWED:
            drmtap_debug_log(ctx,
                "crtc %u scans out %u of the %u-pixel-wide scanout fb, but the layout "
                "is tiled (modifier 0x%llx); reporting the whole fb (narrowing a tiled "
                "width would mis-address the deswizzle)",
                ctx->crtc_id, hdisplay, fb_width, (unsigned long long)modifier);
            break;
        case DRMTAP_SCANOUT_SCALING_NOT_NARROWED:
            drmtap_debug_log(ctx,
                "crtc %u plane scales %ux%u -> %ux%u, so the whole %u-pixel-wide fb is "
                "displayed; reporting it unchanged (this is not pitch padding)",
                ctx->crtc_id, rect.src_w, rect.src_h, rect.crtc_w, rect.crtc_h,
                fb_width);
            break;
        case DRMTAP_SCANOUT_OFFSET_UNSUPPORTED:
            drmtap_debug_log(ctx,
                "crtc %u plane reads from +%u+%u inside a %u-pixel-wide scanout fb "
                "(mode %ux%u); reporting the whole fb (the frame descriptor has no "
                "crop origin)",
                ctx->crtc_id, rect.src_x, rect.src_y, fb_width, hdisplay, vdisplay);
            break;
        case DRMTAP_SCANOUT_NO_PLANE_RECT:
            drmtap_debug_log(ctx,
                "crtc %u has a %u-pixel-wide scanout fb for a %u-pixel mode, but the "
                "plane SRC rect is unreadable, so pitch padding cannot be told from a "
                "scaling plane; reporting the whole fb",
                ctx->crtc_id, fb_width, hdisplay);
            break;
        case DRMTAP_SCANOUT_AS_IS:
            break;
        }
    }

    ctx->sw_key_crtc = ctx->crtc_id;
    ctx->sw_key_hdisplay = hdisplay;
    ctx->sw_key_fb_width = fb_width;
    ctx->sw_key_modifier = modifier;
    ctx->sw_cached_width = out;
    ctx->sw_cached = 1;
    return out;
}

/* ========================================================================= */
/* Multi-plane scanout (0.6.0)                                               */
/* ========================================================================= */

/* Minimum bytes per pixel of plane 0 of a layer, for the stride-covers-width
 * bound on input that crossed a process boundary. Unlike format_min_bpp below it
 * knows the YUV and 16-bit layouts an overlay plane can carry, so it does not
 * reject a real video plane. Unknown fourccs stay at 4. */
static uint32_t layer_min_bpp(uint32_t fourcc) {
    switch (fourcc) {
    case DRM_FORMAT_NV12: case DRM_FORMAT_NV21: case DRM_FORMAT_NV16:
    case DRM_FORMAT_NV61: case DRM_FORMAT_NV24: case DRM_FORMAT_NV42:
    case DRM_FORMAT_YUV420: case DRM_FORMAT_YVU420: case DRM_FORMAT_YUV422:
    case DRM_FORMAT_YVU422: case DRM_FORMAT_YUV444: case DRM_FORMAT_YVU444:
        return 1;
    case DRM_FORMAT_RGB888: case DRM_FORMAT_BGR888:
        return 3;
    case DRM_FORMAT_P010: case DRM_FORMAT_P012: case DRM_FORMAT_P016:
    case DRM_FORMAT_YUYV: case DRM_FORMAT_YVYU: case DRM_FORMAT_UYVY:
    case DRM_FORMAT_VYUY: case DRM_FORMAT_RGB565: case DRM_FORMAT_BGR565:
        return 2;
    case 0x38345258u: case 0x38345241u: case 0x38344258u: case 0x38344241u: /* XR48.. */
    case 0x48345258u: case 0x48345241u: case 0x48344258u: case 0x48344241u: /* XR4H.. */
        return 8;
    default:
        return 4;
    }
}

/* Rows of plane p of a known YUV layout of `height` rows, so a chroma plane is
 * bounded by its whole extent and not only by one row: since 0.6.0 a packet can
 * put it in an fd of its own, and the GPU import reads all of it. 0 for a layout
 * this does not know (a compression/clear-colour aux plane, whose extent is a
 * format-specific fraction): the caller then keeps the one-row bound. */
static uint32_t layer_plane_rows(uint32_t fourcc, uint32_t p, uint32_t height) {
    uint32_t half = (height + 1) / 2;
    switch (fourcc) {
    case DRM_FORMAT_NV12: case DRM_FORMAT_NV21:
    case DRM_FORMAT_P010: case DRM_FORMAT_P012: case DRM_FORMAT_P016:
        return p == 1 ? half : 0;
    case DRM_FORMAT_YUV420: case DRM_FORMAT_YVU420:
        return (p == 1 || p == 2) ? half : 0;
    case DRM_FORMAT_NV16: case DRM_FORMAT_NV61: case DRM_FORMAT_NV24: case DRM_FORMAT_NV42:
        return p == 1 ? height : 0;
    case DRM_FORMAT_YUV422: case DRM_FORMAT_YVU422:
    case DRM_FORMAT_YUV444: case DRM_FORMAT_YVU444:
        return (p == 1 || p == 2) ? height : 0;
    default:
        return 0;
    }
}

/* The narrowest row of plane p >= 1 of a known YUV layout `width` pixels wide, in
 * bytes; 1 for any other plane (a compression or clear-colour aux plane, whose
 * width is format-specific): a used plane has at least one byte per row. */
static uint32_t layer_plane_min_pitch(uint32_t fourcc, uint32_t p, uint32_t width) {
    uint64_t half = ((uint64_t)width + 1) / 2;
    uint64_t v = 1;
    switch (fourcc) {
    case DRM_FORMAT_NV12: case DRM_FORMAT_NV21: case DRM_FORMAT_NV16: case DRM_FORMAT_NV61:
        v = p == 1 ? half * 2 : 1;
        break;
    case DRM_FORMAT_NV24: case DRM_FORMAT_NV42:
        v = p == 1 ? (uint64_t)width * 2 : 1;
        break;
    case DRM_FORMAT_P010: case DRM_FORMAT_P012: case DRM_FORMAT_P016:
        v = p == 1 ? half * 4 : 1;
        break;
    case DRM_FORMAT_YUV420: case DRM_FORMAT_YVU420: case DRM_FORMAT_YUV422: case DRM_FORMAT_YVU422:
        v = (p == 1 || p == 2) ? half : 1;
        break;
    case DRM_FORMAT_YUV444: case DRM_FORMAT_YVU444:
        v = (p == 1 || p == 2) ? width : 1;
        break;
    default:
        break;
    }
    return v > 0xffffffffu ? 0xffffffffu : (uint32_t)v;
}

/* The fds of a set that crossed a process boundary (a packet, or the helper's
 * reply). Each must be a genuine DMA-BUF -- immutable in size, so the bound cannot
 * be raced by a peer that shrinks it -- and large enough for every plane that
 * indexes it, each plane against the size of its OWN fd (lseek): plane 0 needs
 * offset + pitch * height, a chroma plane of a known YUV layout offset + pitch *
 * its rows, and any other auxiliary plane (CCS, clear colour) at least offset +
 * one row, the rule drmtap_convert_dmabuf applies. Runs before any import or mmap. */
static int validate_layer_fds(drmtap_ctx *ctx, const drmtap_layer_set *s) {
    off_t size[DRMTAP_LAYERS_MAX_FDS];
    if (s->nfds == 0 || s->nfds > DRMTAP_LAYERS_MAX_FDS) {
        drmtap_set_error(ctx, "layers: %u fds", s->nfds);
        return -EINVAL;
    }
    for (uint32_t i = 0; i < s->nfds; i++) {
        int fd = s->fds[i];
        if (fd < 0 || dmabuf_sync_start(fd) != 0) {
            drmtap_set_error(ctx, "layers: fd %u is not a dma-buf", i);
            return -EINVAL;
        }
        off_t sz = lseek(fd, 0, SEEK_END);
        if (sz <= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0) {
                sz = st.st_size;
            }
        }
        dmabuf_sync_end(fd);
        if (sz <= 0) {
            drmtap_set_error(ctx, "layers: the size of dma-buf %u is unknown", i);
            return -EINVAL;
        }
        size[i] = sz;
    }
    for (uint32_t i = 0; i < s->count; i++) {
        const drmtap_layer *l = &s->layers[i];
        if ((uint64_t)l->fb_width * layer_min_bpp(l->format) > l->pitches[0]) {
            drmtap_set_error(ctx, "layers: layer %u stride %u too small for width %u "
                             "(fourcc %s)", i, l->pitches[0], l->fb_width,
                             drmtap_fourcc_str(l->format, (char[11]){0}));
            return -EINVAL;
        }
        for (uint32_t p = 0; p < l->num_planes && p < 4; p++) {
            if (p > 0 && l->pitches[p] < layer_plane_min_pitch(l->format, p, l->fb_width)) {
                drmtap_set_error(ctx, "layers: layer %u plane %u stride %u too small for "
                                 "width %u (fourcc %s)", i, p, l->pitches[p], l->fb_width,
                                 drmtap_fourcc_str(l->format, (char[11]){0}));
                return -EINVAL;
            }
            uint32_t rows = (p == 0) ? l->fb_height
                                     : layer_plane_rows(l->format, p, l->fb_height);
            if (rows == 0) {
                rows = 1;
            }
            uint64_t need = (uint64_t)l->offsets[p] + (uint64_t)l->pitches[p] * rows;
            if (l->fd_index[p] >= s->nfds || need > (uint64_t)size[l->fd_index[p]]) {
                drmtap_set_error(ctx, "layers: layer %u plane %u exceeds its dma-buf "
                                 "(need %llu bytes)", i, p, (unsigned long long)need);
                return -EINVAL;
            }
        }
    }
    return 0;
}

/* CPU composition of linear RGB layers: map each fd once (read-only, inside a
 * DMA-BUF sync), compose, unmap. -ENOTSUP when a layer needs the GPU. */
static int compose_layers_cpu(drmtap_ctx *ctx, const drmtap_layer_set *s, void **out_data) {
    int invalid_is_linear = !drmtap_gpu_egl_available(ctx);
    for (uint32_t i = 0; i < s->count; i++) {
        const drmtap_layer *l = &s->layers[i];
        if (!drmtap_compose_cpu_supported(l, invalid_is_linear)) {
            drmtap_set_error(ctx, "layer %u (fourcc %s, modifier 0x%llx, rotation 0x%x) "
                             "needs the GPU to compose and the EGL backend is unavailable "
                             "or failed", i, drmtap_fourcc_str(l->format, (char[11]){0}),
                             (unsigned long long)l->modifier, l->rotation);
            return -ENOTSUP;
        }
    }
    size_t need[DRMTAP_LAYERS_MAX_FDS] = {0};
    void *map[DRMTAP_LAYERS_MAX_FDS];
    int synced[DRMTAP_LAYERS_MAX_FDS] = {0};
    for (uint32_t i = 0; i < DRMTAP_LAYERS_MAX_FDS; i++) {
        map[i] = MAP_FAILED;
    }
    for (uint32_t i = 0; i < s->count; i++) {
        const drmtap_layer *l = &s->layers[i];
        size_t n = (size_t)l->offsets[0] + (size_t)l->pitches[0] * l->fb_height;
        if (n > need[l->fd_index[0]]) {
            need[l->fd_index[0]] = n;
        }
    }
    int ret = 0;
    const uint8_t *bases[DRMTAP_MAX_LAYERS];
    for (uint32_t f = 0; f < s->nfds && f < DRMTAP_LAYERS_MAX_FDS; f++) {
        if (need[f] == 0) {
            continue;
        }
        synced[f] = (dmabuf_sync_start(s->fds[f]) == 0);
        map[f] = mmap(NULL, need[f], PROT_READ, MAP_SHARED, s->fds[f], 0);
        if (map[f] == MAP_FAILED) {
            ret = errno ? -errno : -EIO;
            drmtap_set_error(ctx, "layers: mmap of %zu bytes failed: %s", need[f],
                             strerror(-ret));
            goto out;
        }
    }
    for (uint32_t i = 0; i < s->count; i++) {
        bases[i] = (const uint8_t *)map[s->layers[i].fd_index[0]] + s->layers[i].offsets[0];
    }
    void *out = NULL;
    ret = drmtap_ensure_out(ctx, (size_t)s->crtc_w * s->crtc_h * 4u, &out);
    if (ret != 0) {
        goto out;
    }
    ret = drmtap_compose_cpu(s->layers, s->count, bases, s->crtc_w, s->crtc_h, out,
                             s->crtc_w * 4u);
    if (ret == 0) {
        *out_data = out;
        drmtap_debug_log(ctx, "cpu: composed %u layers into %ux%u", s->count, s->crtc_w,
                         s->crtc_h);
    }
out:
    for (uint32_t f = 0; f < DRMTAP_LAYERS_MAX_FDS; f++) {
        if (map[f] != MAP_FAILED) {
            munmap(map[f], need[f]);
        }
        if (synced[f]) {
            dmabuf_sync_end(s->fds[f]);
        }
    }
    return ret;
}

/* Turn a set of layers (fds attached) into one CRTC-sized XRGB8888 frame: EGL
 * first, then the CPU for layers it can read. -ENOTSUP when neither can. */
static int compose_layers(drmtap_ctx *ctx, const drmtap_layer_set *s,
                          drmtap_frame_info *frame) {
    int dim = drmtap_validate_fb_dims(s->crtc_w, s->crtc_h);
    if (dim != 0) {
        drmtap_set_error(ctx, "layers: CRTC size %ux%u rejected", s->crtc_w, s->crtc_h);
        return dim;
    }
    void *out = NULL;
    size_t sz = 0;
    int ret = -ENOTSUP;
    if (drmtap_gpu_egl_available(ctx)) {
        ret = drmtap_gpu_egl_compose(ctx, s, &out, &sz);
        if (ret == -ENOSPC) {
            return ret;
        }
        if (ret != 0) {
            drmtap_debug_log(ctx, "compose: EGL failed (%d), trying the CPU", ret);
        }
    }
    if (ret != 0) {
        ret = compose_layers_cpu(ctx, s, &out);
        if (ret != 0) {
            return ret;
        }
    }
    memset(frame, 0, sizeof(*frame));
    frame->data = out;
    frame->dma_buf_fd = -1;
    frame->width = s->crtc_w;
    frame->height = s->crtc_h;
    frame->stride = s->crtc_w * 4u;
    frame->format = DRM_FORMAT_XRGB8888;
    frame->modifier = DRM_FORMAT_MOD_LINEAR;
    int pi = drmtap_layers_primary(s);
    frame->fb_id = s->layers[pi >= 0 ? pi : 0].fb_id;
    return 0;
}

/* The layers from the helper (CMD_GRAB_LAYERS), decoded and checked like any
 * other packet that crossed a process boundary. Takes over the received fds. */
static int helper_layers(drmtap_ctx *ctx, drmtap_layer_set *s) {
    uint8_t buf[DRMTAP_PKT_MAX_LEN];
    size_t len = 0;
    int fds[DRMTAP_LAYERS_MAX_FDS];
    uint32_t nfds = 0;
    int r = drmtap_helper_grab_layers(ctx, buf, sizeof(buf), &len, fds,
                                      DRMTAP_LAYERS_MAX_FDS, &nfds);
    if (r < 0) {
        return r;
    }
    r = drmtap_packet_decode(buf, len, nfds, s);
    if (r < 0) {
        for (uint32_t i = 0; i < nfds; i++) {
            close(fds[i]);
        }
        drmtap_set_error(ctx, "helper sent a malformed layer packet (%d)", r);
        return -EPROTO;
    }
    for (uint32_t i = 0; i < nfds; i++) {
        s->fds[i] = fds[i];
    }
    r = validate_layer_fds(ctx, s);
    if (r < 0) {
        drmtap_layers_close_fds(s);
        return r;
    }
    ctx->cur_hdr_eotf = s->hdr_eotf;
    ctx->cur_hdr_max_nits = s->hdr_max_nits;
    return 0;
}

/* grab_desc after a long underlay, through the helper. CMD_GRAB answers for the
 * primary plane, which in an underlay is the window, so the covering layer comes
 * from CMD_GRAB_LAYERS, with its own buffer and its own layout staged in ctx->fb2_*.
 * When that is not possible the answer stays -EAGAIN, as during the first
 * DRMTAP_UNDERLAY_EAGAIN_MS: a descriptor never pairs one plane's metadata with the
 * buffer of another plane. */
static int helper_cover_layer(drmtap_ctx *ctx, uint32_t plane_id, helper_grab_result_t *res) {
    drmtap_layer_set s;
    int r = helper_layers(ctx, &s);
    if (r < 0) {
        drmtap_set_error(ctx, "crtc %u: underlay, and the helper could not export the "
                         "covering plane %u (%s). Keep the previous frame, or capture "
                         "with drmtap_grab_packet()", ctx->crtc_id, plane_id, strerror(-r));
        return -EAGAIN;
    }
    const drmtap_layer *l = NULL;
    for (uint32_t i = 0; i < s.count; i++) {
        if (s.layers[i].plane_id == plane_id) {
            l = &s.layers[i];
        }
    }
    if (l && !drmtap_layer_covers(l, s.crtc_w, s.crtc_h)) {
        l = NULL;   /* the helper read a newer commit: that plane no longer covers */
    }
    int one_fd = l != NULL;
    for (uint32_t p = 1; l && p < l->num_planes; p++) {
        one_fd = one_fd && l->fd_index[p] == l->fd_index[0];
    }
    int fd = (l && one_fd) ? fcntl(s.fds[l->fd_index[0]], F_DUPFD_CLOEXEC, 0) : -1;
    if (fd < 0) {
        drmtap_layers_close_fds(&s);
        drmtap_set_error(ctx, "crtc %u: underlay, and the helper's planes do not give the "
                         "covering plane %u as one buffer (%s). Keep the previous frame, "
                         "or capture with drmtap_grab_packet()", ctx->crtc_id, plane_id,
                         !l ? "it no longer covers the CRTC"
                            : !one_fd ? "its planes span buffers" : strerror(errno));
        return -EAGAIN;
    }
    memset(res, 0, sizeof(*res));
    res->dmabuf_fd = fd;
    res->wire.width = l->fb_width;
    res->wire.height = l->fb_height;
    res->wire.stride = l->pitches[0];
    res->wire.format = l->format;
    res->wire.modifier = l->modifier;
    res->wire.fb_id = l->fb_id;
    res->wire.flags = HELPER_FLAG_HAS_DMABUF;
    res->wire.hdr_eotf = s.hdr_eotf;
    res->wire.hdr_max_nits = s.hdr_max_nits;
    ctx->fb2_num_planes = (int)l->num_planes;
    ctx->fb2_fb_id = l->fb_id;
    for (uint32_t p = 0; p < 4; p++) {
        ctx->fb2_pitches[p] = p < l->num_planes ? l->pitches[p] : 0;
        ctx->fb2_offsets[p] = p < l->num_planes ? l->offsets[p] : 0;
    }
    /* the crop rect of the buffer the helper sent, not of our earlier read */
    drmtap_layer_plane_rect(l, &ctx->sw_override);
    drmtap_layers_close_fds(&s);
    return 0;
}

/* The helper read its own framebuffer, which is a newer one when the compositor
 * flipped since our GetFB2: stage the layout of the fb it actually sent. Only a
 * flip of the same buffer chain (same size, format and modifier) is taken; any
 * other fb is another plane's buffer, and that frame is skipped. */
static int helper_restage_fb(drmtap_ctx *ctx, const drmModeFB2 *asked, uint32_t fb_id) {
    drmModeFB2 *f = drmModeGetFB2(ctx->drm_fd, fb_id);
    if (!f) {
        drmtap_set_error(ctx, "the helper sent fb %u, which is already gone: frame "
                         "skipped", fb_id);
        return -EAGAIN;
    }
    if (f->width != asked->width || f->height != asked->height ||
        f->pixel_format != asked->pixel_format || f->modifier != asked->modifier) {
        drmtap_set_error(ctx, "the helper sent fb %u (%ux%u %s), not a flip of the "
                         "fb %u asked for (%ux%u %s): frame skipped", fb_id, f->width,
                         f->height, drmtap_fourcc_str(f->pixel_format, (char[11]){0}), asked->fb_id,
                         asked->width, asked->height, drmtap_fourcc_str(asked->pixel_format, (char[11]){0}));
        for (int p = 0; p < 4; p++) {
            int seen = 0;
            for (int q = 0; q < p; q++) {
                seen = seen || f->handles[q] == f->handles[p];
            }
            if (f->handles[p] && !seen) {
                drmtap_gem_close(ctx, f->handles[p]);
            }
        }
        drmModeFreeFB2(f);
        return -EAGAIN;
    }
    ctx->fb2_num_planes = 0;
    ctx->fb2_fb_id = fb_id;
    for (int p = 0; p < 4; p++) {
        ctx->fb2_pitches[p] = f->pitches[p];
        ctx->fb2_offsets[p] = f->offsets[p];
        if (f->handles[p] || f->pitches[p]) {
            ctx->fb2_num_planes = p + 1;
        }
    }
    uint32_t closed[4] = {0, 0, 0, 0};
    for (int p = 0; p < 4; p++) {
        uint32_t h = f->handles[p];
        int seen = 0;
        for (int q = 0; q < p; q++) {
            seen = seen || closed[q] == h;
        }
        if (h && !seen) {
            drmtap_gem_close(ctx, h);
        }
        closed[p] = h;
    }
    drmModeFreeFB2(f);
    return 0;
}

/* Capture every lit layer and compose them, for the in-process consumers. Exports
 * the buffers itself with CAP_SYS_ADMIN; otherwise asks the helper when
 * allow_helper. -E2BIG: more lit planes than a frame carries. */
static int grab_composed(drmtap_ctx *ctx, drmtap_frame_info *frame, int allow_helper) {
    drmtap_layer_set s;
    int retried = 0;
    int n = drmtap_kms_capture_layers(ctx->drm_fd, &ctx->layer_cache, ctx->crtc_id, &s,
                                      &retried);
    if (n == -EACCES) {
        if (!allow_helper) {
            return -EACCES;
        }
        int r = helper_layers(ctx, &s);
        if (r < 0) {
            return r;
        }
    } else if (n < 0) {
        drmtap_set_error(ctx, "reading the planes of crtc %u: %s", ctx->crtc_id,
                         strerror(-n));
        return n;
    } else if (n == 0) {
        drmtap_set_error(ctx, "No active plane found for capture");
        return -ENODEV;
    } else if (s.lit > s.count) {
        return -E2BIG;
    } else {
        int r = drmtap_kms_crtc_size(ctx->drm_fd, ctx->crtc_id, &s.crtc_w, &s.crtc_h);
        if (r < 0) {
            drmtap_layers_close_fds(&s);
            drmtap_set_error(ctx, "crtc %u has no mode to compose into", ctx->crtc_id);
            return r;
        }
        s.hdr_eotf = ctx->cur_hdr_eotf;
        s.hdr_max_nits = ctx->cur_hdr_max_nits;
    }
    if (retried) {
        drmtap_debug_log(ctx, "layers: a plane flipped during the export; read them again");
    }
    int ret = compose_layers(ctx, &s, frame);
    if (ret == 0) {
        int pi = drmtap_layers_primary(&s);
        ctx->grab_plane_id = s.layers[pi >= 0 ? pi : 0].plane_id;
        ctx->last_frame_composed = 1;
    }
    drmtap_layers_close_fds(&s);
    return ret;
}

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A single plane that could not be composed for a reason of its own (no composer for
 * its layout, an import the driver refused) is not tried again for 2 s while its
 * shape stays the same: without that every frame exported it twice, as layers and
 * then as one buffer. The 2 s also cover a format or modifier change the shape does
 * not see. Any other error is retried on the next frame. */
#define DRMTAP_ONE_NOCOMPOSE_MS 2000
static int one_layer_should_compose(drmtap_ctx *ctx, const drmtap_layer *l,
                                    uint32_t w, uint32_t h) {
    return ctx->one_nocompose_key != drmtap_layer_shape_key(l, w, h) ||
           mono_ms() >= ctx->one_nocompose_until_ms;
}
static void one_layer_composed(drmtap_ctx *ctx, const drmtap_layer *l, uint32_t w,
                               uint32_t h, int err) {
    if (err == 0) {
        ctx->one_nocompose_key = 0;
    } else if (err == -ENOTSUP || err == -EIO || err == -EPROTO) {
        ctx->one_nocompose_key = drmtap_layer_shape_key(l, w, h);
        ctx->one_nocompose_until_ms = mono_ms() + DRMTAP_ONE_NOCOMPOSE_MS;
    }
}

/* A single plane that is not the screen could not be composed: its buffer is
 * returned at its own size, as before 0.6.1. Said once per context. */
static void log_one_layer_fallback(drmtap_ctx *ctx, int err) {
    if (ctx->logged_one_layer_fallback) {
        return;
    }
    ctx->logged_one_layer_fallback = 1;
    drmtap_debug_log(ctx, "crtc %u: its one plane is stretched or smaller than the "
                     "screen and could not be composed to the screen size (%s: %s); "
                     "the frame is that plane at its own size. Said once per context",
                     ctx->crtc_id, strerror(-err), drmtap_error(ctx));
}

/* Errors after which a multi-plane frame is still captured, from its primary
 * plane alone: nothing could compose it (no EGL and a layer the CPU cannot read,
 * an import the driver refused, a helper older than 0.6.0 or one that could not
 * export), or more planes than a frame carries. Anything else is returned. */
static int layer_fallback_ok(int err) {
    return err == -ENOTSUP || err == -E2BIG || err == -EIO || err == -EPROTO;
}

static void log_layer_fallback(drmtap_ctx *ctx, int lit, int err) {
    if (ctx->logged_layer_fallback) {
        return;
    }
    ctx->logged_layer_fallback = 1;
    const char *why = drmtap_error(ctx);
    drmtap_debug_log(ctx,
        "a frame of %d planes on crtc %u could not be composed (%s: %s); "
        "capturing one plane alone (the primary, or the plane that covers the screen when "
        "the primary is a window under it), so windows on the other planes are missing. "
        "Said once per context",
        lit, ctx->crtc_id, err == -E2BIG ? "more lit planes than a frame carries"
                                         : strerror(-err),
        (err != -E2BIG && why) ? why : "-");
}

// Internal capture that populates frame_info
// If do_mmap is true, also maps the pixel data to frame->data.
// one_buffer: the caller needs ONE scanout buffer (drmtap_grab_desc), so a
// multi-plane frame is never composed here (see that function for what it gets).
static int do_grab(drmtap_ctx *ctx, drmtap_frame_info *frame, int do_mmap, int one_buffer) {
    int ret;
    frame_priv_t *priv = NULL;
    drmModeFB2 *fb2 = NULL;
    int prime_fd = -1;

    if (ctx->is_render_only) {
        drmtap_set_error(ctx, "context is render-only (drmtap_open_render); "
                         "grab needs a KMS context from drmtap_open");
        return -ENOTSUP;
    }
    ctx->sw_override_on = 0;

    /* Step 1: which planes are lit (0.6.0). One lit plane is the single-buffer
     * capture below, unless it is stretched or smaller than the CRTC and the caller
     * wants pixels (composed, 0.6.1). Several are composed, unless the caller needs ONE
     * buffer. Without the plane properties (no atomic cap) the legacy primary
     * lookup runs, as before 0.6.0. The fb_id comes from the same read: never
     * cached across frames. */
    drmtap_layer_set ls;
    int nl = read_layers(ctx, &ls);
    read_hdr_for_current_crtc(ctx);
    uint32_t plane_id = 0;
    uint32_t fb_id = 0;
    int cover = -1;   /* grab_desc after a long underlay: the layer it returns */
    if (one_buffer && nl <= 1) {
        ctx->underlay_since_ms = 0;
        ctx->underlay_cover_logged = 0;
    }
    if (nl > 1) {
        int pi = drmtap_layers_primary(&ls);
        if (one_buffer) {
            uint32_t cw = 0, ch = 0;
            int sr = drmtap_kms_crtc_size(ctx->drm_fd, ctx->crtc_id, &cw, &ch);
            if (sr != 0) {
                /* Not an underlay: without the mode nothing can be decided, and a
                 * -EAGAIN here would never end. A modeset in flight answers this. */
                drmtap_set_error(ctx, "crtc %u blends %d planes and its mode cannot be read "
                                 "(%s): no frame", ctx->crtc_id, nl, strerror(-sr));
                return sr < 0 ? sr : -ENODEV;
            }
            int underlay = pi < 0 || !drmtap_layer_covers(&ls.layers[pi], cw, ch);
            if (underlay) {
                /* the topmost layer that covers the CRTC (the desktop with its hole) */
                for (int i = (int)ls.count - 1; i >= 0 && cover < 0; i--) {
                    if (drmtap_layer_covers(&ls.layers[i], cw, ch)) {
                        cover = i;
                    }
                }
            }
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            int64_t now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            int d = drmtap_underlay_decide(&ctx->underlay_since_ms, now_ms, underlay,
                                           cover >= 0, DRMTAP_UNDERLAY_EAGAIN_MS);
            if (d == DRMTAP_UNDERLAY_EAGAIN) {
                drmtap_set_error(ctx,
                    "crtc %u blends %d planes and its primary plane does not cover it "
                    "(a window under the desktop): one buffer cannot describe this "
                    "frame. Keep the previous frame, or capture with drmtap_grab_packet()",
                    ctx->crtc_id, nl);
                return -EAGAIN;
            }
            if (d == DRMTAP_UNDERLAY_NONE) {
                cover = -1;
                ctx->underlay_cover_logged = 0;
            } else if (!ctx->underlay_cover_logged) {
                ctx->underlay_cover_logged = 1;
                drmtap_debug_log(ctx, "crtc %u: underlay for over %d ms; grab_desc returns "
                                 "the covering plane %u (the window shows as its hole)",
                                 ctx->crtc_id, DRMTAP_UNDERLAY_EAGAIN_MS,
                                 ls.layers[cover].plane_id);
            }
        } else {
            int r = grab_composed(ctx, frame, 1);
            if (r == 0) {
                return 0;
            }
            if (!layer_fallback_ok(r)) {
                return r;
            }
            log_layer_fallback(ctx, nl, r);
            /* One plane alone stands for the screen: never the window of an underlay. */
            uint32_t cw = 0, ch = 0;
            int si = drmtap_kms_crtc_size(ctx->drm_fd, ctx->crtc_id, &cw, &ch) == 0
                     ? drmtap_layers_stand_in(&ls, cw, ch) : -1;
            if (si < 0) {
                drmtap_set_error(ctx, "crtc %u: %d planes could not be composed (%s) and no "
                                 "single plane covers the screen: no frame", ctx->crtc_id,
                                 nl, strerror(-r));
                return r;
            }
            if (si != pi) {
                cover = si;   /* the covering layer, also through the helper */
            }
        }
        int pick = cover >= 0 ? cover : (pi >= 0 ? pi : 0);
        plane_id = ls.layers[pick].plane_id;
        fb_id = ls.layers[pick].fb_id;
        if (cover >= 0) {
            drmtap_layer_plane_rect(&ls.layers[cover], &ctx->sw_override);
            ctx->sw_override_on = 1;
        }
    } else if (nl == 1) {
        uint32_t cw = 0, ch = 0;
        if (do_mmap &&
            drmtap_kms_crtc_size(ctx->drm_fd, ctx->crtc_id, &cw, &ch) == 0 &&
            !drmtap_layer_is_screen(&ls.layers[0], cw, ch) &&
            one_layer_should_compose(ctx, &ls.layers[0], cw, ch)) {
            /* One plane that is not the screen as it is (stretched, or smaller), on a
             * path that returns pixels: compose it to the CRTC size like several
             * planes, else the frame is the size of its buffer. When that cannot be
             * done, its buffer as before. The zero-copy paths (drmtap_grab,
             * drmtap_grab_desc) keep returning the buffer: their contract is an fd.
             * Any compose error falls back: a frame of the old size beats no frame. */
            int r = grab_composed(ctx, frame, 1);
            one_layer_composed(ctx, &ls.layers[0], cw, ch, r);
            if (r == 0) {
                return 0;
            }
            log_one_layer_fallback(ctx, r);
        }
        plane_id = ls.layers[0].plane_id;
        fb_id = ls.layers[0].fb_id;
    } else if (nl == 0) {
        drmtap_set_error(ctx, "No active plane found for capture");
        return -ENODEV;
    } else {
        plane_id = find_primary_plane(ctx);
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
        fb_id = plane->fb_id;
        drmModeFreePlane(plane);
    }

    /* Step 3: GetFB2 → format, handles, strides, modifier */
    fb2 = drmModeGetFB2(ctx->drm_fd, fb_id);
    if (!fb2) {
        ret = -errno;
        drmtap_set_error(ctx, "drmModeGetFB2(%u) failed: %s",
                         fb_id, strerror(errno));
        return ret;
    }

    drmtap_debug_log(ctx, "FB2: %ux%u fmt=%s modifier=0x%lx",
                     fb2->width, fb2->height,
                     drmtap_fourcc_str(fb2->pixel_format, (char[11]){0}),
                     (unsigned long)fb2->modifier);

    /* drmModeGetFB2 minted a handle somebody has to close (see drmtap_gem_close).
     * Hold it here until priv adopts it, so every error return below can close it
     * without knowing how far the function got. The fast path already did this;
     * three returns on THIS path did not, and leaked one handle per grab. Set to 0
     * the moment ownership moves, so nothing is closed twice. */
    uint32_t pending_gem = fb2->handles[0];
    close_auxiliary_gem_handles(ctx, fb2);

    /* Bound the DIMENSIONS too, not just stride*height: width is unconstrained by
     * validate_fb_size, and every converted output is sized width*height*4. */
    ret = drmtap_validate_fb_dims(fb2->width, fb2->height);
    if (ret == 0) {
        ret = validate_fb_size(fb2->pitches[0], fb2->height);
    }
    if (ret != 0) {
        drmtap_set_error(ctx, "rejecting framebuffer geometry %ux%u stride=%u",
                         fb2->width, fb2->height, fb2->pitches[0]);
        goto cleanup;
    }

    /* Cache multi-plane info for EGL CCS import */
    ctx->fb2_num_planes = 0;
    ctx->fb2_fb_id = fb_id;
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
            pending_gem = 0;
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
        if (cover >= 0) {
            /* grab_desc after a long underlay: the covering layer, never CMD_GRAB's
             * primary (the window) under the covering layer's metadata. */
            ret = helper_cover_layer(ctx, ls.layers[cover].plane_id, &hresult);
            if (ret < 0) {
                drmModeFreeFB2(fb2);
                return ret;
            }
        } else {
            ret = drmtap_helper_grab(ctx, &hresult, pixel_buf, buf_size);
            if (ret == 0 && hresult.wire.fb_id != fb_id) {
                int rr = helper_restage_fb(ctx, fb2, hresult.wire.fb_id);
                if (rr < 0) {
                    if (hresult.dmabuf_fd >= 0) {
                        close(hresult.dmabuf_fd);
                    }
                    drmModeFreeFB2(fb2);
                    return rr;
                }
            }
        }
        if (ret < 0) {
#ifdef DRMTAP_NO_HELPER
            /* Built with -Dhelper=disabled: the stub has already said the one
             * useful thing, and telling the user to install a helper this build
             * cannot spawn would send them the wrong way. */
            ret = -EACCES;
            goto cleanup;
#else
            drmtap_set_error(ctx,
                "No CAP_SYS_ADMIN and helper failed (ret=%d). Install the "
                "helper, restricting it first so the capability is not "
                "world-usable: sudo cp drmtap-helper /usr/local/bin/ && "
                "sudo chown root:<capture-group> /usr/local/bin/drmtap-helper && "
                "sudo chmod 0750 /usr/local/bin/drmtap-helper && "
                "sudo setcap cap_sys_admin+ep /usr/local/bin/drmtap-helper "
                "(see SECURITY.md)",
                ret);
            ret = -EACCES;
            goto cleanup;
#endif
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
        /* Narrow to what the CRTC scans out, exactly as the direct path does.
         * The helper reports the framebuffer, and the padding is the driver's,
         * so it is present on this path too. */
        frame->width = drmtap_scanout_width(ctx, hresult.wire.width,
                                            hresult.wire.modifier);
        frame->height = hresult.wire.height;
        frame->stride = hresult.wire.stride;
        frame->format = hresult.wire.format;
        frame->modifier = hresult.wire.modifier;
        frame->fb_id = hresult.wire.fb_id;
        /* HDR transfer the helper read from the connector — drives tone-mapping. */
        ctx->cur_hdr_eotf = hresult.wire.hdr_eotf;
        ctx->cur_hdr_max_nits = hresult.wire.hdr_max_nits;

        /* The helper validates its own geometry, but it sends it over the wire —
         * re-check before we mmap/size anything from those values. BOTH checks are
         * needed: validate_fb_size bounds stride*height (what we mmap/receive) and
         * drmtap_validate_fb_dims bounds width, which the first one does not constrain at
         * all. An unbounded width reaches the conversion paths, where every output
         * is sized width*height*4 and that product can wrap. */
        ret = drmtap_validate_fb_dims(frame->width, frame->height);
        if (ret == 0) {
            ret = validate_fb_size(frame->stride, frame->height);
        }
        if (ret == 0 &&
            (uint64_t)frame->width * format_min_bpp(frame->format) > frame->stride) {
            /* THIRD bound, and the one the other two do not imply: the stride must
             * actually cover width pixels. Without it a wire frame claiming
             * width*bpp > stride reaches the per-pixel CPU converters, which read
             * y*stride + width*bpp per row with no size argument, and the last rows
             * run off the end of the receive buffer. drmtap_convert_dmabuf applies
             * the same check to its IPC descriptor; the two trust boundaries must be
             * bounded alike, and this one was not. */
            drmtap_set_error(ctx, "helper sent stride %u too small for width %u "
                             "(fourcc %s)", frame->stride, frame->width,
                             drmtap_fourcc_str(frame->format, (char[11]){0}));
            ret = -EINVAL;
        }
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
            /* Propagate ANY processing failure -- a virgl readback that produced no
             * pixels, or an unconvertible CCS/compressed scanout that returned
             * -ENOTSUP -- instead of handing the caller a black or still-compressed
             * frame reported as valid. (Previously only virgl failures propagated.) */
            if (pret != 0) {
                /* Release everything this frame acquired (the DMA-BUF fd, the
                 * mmap and priv) — a failed grab is not released by the caller,
                 * so returning here without cleanup would leak them each grab. */
                drmtap_frame_release(ctx, frame);
                return pret;
            }
            /* Only a grab that produced a frame names the plane drmtap_plane_rotation() answers for. */
            ctx->grab_plane_id = plane_id;
            ctx->last_frame_composed = 0;
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
            int pr = gpu_auto_process(ctx, pixel_buf, frame, 0);
            if (pr != 0) {
                /* Propagate a processing failure (e.g. an unconvertible CCS
                 * scanout that returned -ENOTSUP) instead of handing back the
                 * undecoded pixels reported as a valid frame. The caller does
                 * not release a frame on error, so release priv here (V2 borrows
                 * ctx->pixel_buf, so this frees priv only). */
                drmModeFreeFB2(fb2);
                drmtap_frame_release(ctx, frame);
                return pr;
            }
        }

        drmModeFreeFB2(fb2);
        /* Only a grab that produced a frame names the plane drmtap_plane_rotation() answers for. */
        ctx->grab_plane_id = plane_id;
        ctx->last_frame_composed = 0;
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
    pending_gem = 0;  /* priv owns it now */
    priv->mapped = MAP_FAILED;
    priv->mapped_size = 0;

    /* Fill frame info */
    memset(frame, 0, sizeof(*frame));
    frame->width = drmtap_scanout_width(ctx, fb2->width,
                                        fb2_effective_modifier(fb2));
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

        /* Standard DMA-BUF mmap path (non-virtio or virtio fallback).
         * No SYNC_START here: the mapping itself is not CPU access, and the
         * only START that may be issued is the recorded one below, which
         * release matches with a SYNC_END. */
        if (mapped == MAP_FAILED) {
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
             * virtio_gpu where the transfer arrives in system RAM asynchronously.
             * This is the ONLY START on this path, so START and END stay 1:1 per
             * frame, including when the mmap above failed and EGL took over. */
            priv->sync_started = (dmabuf_sync_start(prime_fd) == 0);

            priv->mapped = mapped;
            priv->mapped_size = size;
            frame->data = mapped;
            drmtap_debug_log(ctx, "mapped %zu bytes at %p", size, mapped);

            /* Auto-deswizzle tiled framebuffers + format convert */
            int pr = gpu_auto_process(ctx, mapped, frame, 0);
            if (pr != 0) {
                /* Propagate a processing failure (e.g. an unconvertible CCS
                 * scanout that returned -ENOTSUP) instead of handing back the
                 * still-tiled/compressed mmap reported as a valid frame. The
                 * caller does not release a frame on error, so release the fd,
                 * mmap (with its matching SYNC_END) and priv here. */
                drmModeFreeFB2(fb2);
                drmtap_frame_release(ctx, frame);
                return pr;
            }
        }
    }

    drmModeFreeFB2(fb2);
    /* Only a grab that produced a frame names the plane drmtap_plane_rotation() answers for. */
    ctx->grab_plane_id = plane_id;
    ctx->last_frame_composed = 0;
    return 0;

cleanup:
    if (prime_fd >= 0) {
        close(prime_fd);
    }
    if (fb2) {
        drmModeFreeFB2(fb2);
    }
    /* Close the GEM handle. This used to run only `if (priv)`, but two of the paths
     * that reach this label -- drmPrimeHandleToFD failing for anything other than
     * EACCES/EPERM, and the priv calloc failing -- get here BEFORE priv exists, so
     * the handle they minted was leaked once per grab attempt. pending_gem is
     * whatever has not been handed over yet, and is 0 once priv owns it or the
     * helper branch already closed it. */
    if (priv) {
        drmtap_gem_close(ctx, priv->gem_handle);
    } else {
        drmtap_gem_close(ctx, pending_gem);
    }
    free(priv);
    /* Leave the frame owning nothing on this error exit too, matching the
     * drmtap_frame_release() the propagation sites use: priv is now freed, so a
     * caller that defensively releases on a non-zero return gets a harmless no-op
     * instead of a double-free through a dangling frame->_priv. */
    frame->_priv = NULL;
    frame->dma_buf_fd = -1;
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
    void *out = NULL;
    int b = drmtap_ensure_out(ctx, out_size, &out);
    if (b != 0) {
        /* Name it here too. ensure_out only sets a message for the caller-buffer
         * refusal; an allocation failure for the library-owned buffer would otherwise
         * return a bare errno and leave drmtap_error() reporting something older. */
        if (b != -ENOSPC) {
            drmtap_set_error(ctx, "cannot allocate %zu bytes to reduce a %ux%u frame "
                             "to XRGB8888: %s", out_size, frame->width, frame->height,
                             strerror(-b));
        }
        return b;
    }
    uint32_t dst_stride = frame->width * 4u;
    int hdr = (ctx->cur_hdr_eotf == DRMTAP_EOTF_PQ);
    int ret;

    if (is_ar30) {
        if (hdr) {
            ret = drmtap_tonemap_hdr10(data, out,
                    frame->width, frame->height, frame->stride, dst_stride,
                    fmt, ctx->cur_hdr_max_nits);
        } else {
            ret = drmtap_convert_format(data, out,
                    frame->width, frame->height, frame->stride, dst_stride,
                    fmt, DRMTAP_FMT_XR24);
        }
    } else if (is_rgb16) {
        int bgr = (fmt == DRMTAP_FMT_XB48 || fmt == DRMTAP_FMT_AB48);
        ret = drmtap_convert_rgb16(data, out,
                frame->width, frame->height, frame->stride, dst_stride,
                bgr, ctx->cur_hdr_eotf, ctx->cur_hdr_max_nits);
    } else {
        /* Half-float FP16 (XR4H family): linear-light decode + sRGB re-encode. */
        int bgr = (fmt == DRMTAP_FMT_XB4H || fmt == DRMTAP_FMT_AB4H);
        ret = drmtap_convert_rgb16f(data, out,
                frame->width, frame->height, frame->stride, dst_stride, bgr);
    }
    if (ret != 0) return ret;

    drmtap_debug_log(ctx, "auto-process: linear %s -> XRGB8888 (%s)",
                     is_ar30 ? "10-bit" : (is_rgb16f ? "FP16" : "16-bit"),
                     hdr ? "HDR tone-mapped" : "SDR");
    frame->data = out;
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
            /* egl_data is whatever drmtap_ensure_out resolved inside the convert:
             * the caller's output buffer if it set one, otherwise the ctx-owned
             * grow-once buffer (size-capped inside). Either way adopting it is just
             * repointing the frame — no allocation churn, and no copy. */
            frame->data = egl_data;
            /* EGL outputs RGBA/BGRA 8-bit */
            frame->format = DRM_FORMAT_XRGB8888;
            frame->stride = frame->width * 4;
            drmtap_debug_log(ctx, "auto-process: EGL detiled to linear XRGB8888");
            return 0;
        }
        /* An output buffer too small for the frame is a CALLER CONFIGURATION error,
         * not an EGL failure, and must not fall through. The CPU deswizzle below
         * needs stride*height, which is never LESS than the width*height*4 the detile
         * just refused, so it can only fail again -- and it would replace the
         * actionable -ENOSPC (whose message names the exact size required) with
         * whatever the CPU path reports. On a scanout with no CPU mapping, which is
         * precisely the hardware that depends on this detile, that is -ENOTSUP and a
         * message about a missing mapping: the caller is told its GPU is unsupported
         * when in truth its buffer was a few bytes short. */
        if (ret == -ENOSPC) {
            return ret;
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

    /* --- CPU deswizzle for classic tiling --- */
    /* Resolve the destination HERE and not before the EGL attempt above. The CPU
     * deswizzle keeps the SOURCE stride, so it needs stride*height bytes, which on a
     * padded scanout is MORE than the width*height*4 the EGL detile produces (a
     * 60-wide mode on a 256-byte pitch: 555520 vs 520800). Sizing it up front would
     * refuse a caller-supplied output buffer that the EGL path would have filled
     * perfectly, and on the EGL path it also allocated a shadow buffer that was then
     * never used. frame->stride/height are validated at every entry point
     * (validate_fb_size), so this multiply cannot overflow. */
    size_t size = (size_t)frame->stride * frame->height;
    void *out = NULL;
    int bres = drmtap_ensure_out(ctx, size, &out);
    if (bres != 0) {
        if (bres == -EFBIG) {
            drmtap_set_error(ctx,
                "framebuffer too large for deswizzle: %zu bytes (max %zu)",
                size, (size_t)DRMTAP_MAX_FB_BYTES);
        }
        return bres;
    }
    const char *driver = ctx->driver_name;
    if (drmtap_gpu_intel_match(driver) ||
        drmtap_gpu_nvidia_match(driver) ||
        drmtap_gpu_amd_match(driver)) {
        drmtap_debug_log(ctx, "auto-process: %s CPU deswizzle (mod=0x%lx)",
                         driver, (unsigned long)modifier);
        int ret = drmtap_deswizzle(data, out,
                                   frame->width, frame->height,
                                   frame->stride, frame->stride, modifier,
                                   (size_t)frame->stride * frame->height);
        if (ret == -EINVAL) {
            drmtap_set_error(ctx,
                "CPU deswizzle copies 4-byte pixels, and a %u-pixel row does not fit "
                "the %u-byte stride of format 0x%08x", frame->width, frame->stride,
                frame->format);
            return ret;
        }
        if (ret == -ENOTSUP) {
            /* CCS-compressed (or otherwise unsupported) modifier -- the CPU
             * deswizzle cannot decode it and no EGL path produced linear pixels
             * (helper V2 dumb-map, or EGL import/failure). Returning the raw
             * compressed bytes relabelled LINEAR would hand the caller a corrupt
             * frame reported as valid (drmtap_convert_dmabuf would forward garbage
             * instead of failing over). Fail closed so the caller ends the stream
             * and falls back (e.g. to PipeWire) instead. */
            drmtap_debug_log(ctx,
                "auto-process: modifier 0x%lx has no CPU deswizzle here and the "
                "GPU detile did not run -- failing closed",
                (unsigned long)modifier);
#ifdef HAVE_EGL
            /* EGL IS compiled in, so the detile was skipped or it failed at
             * runtime: GL is not loaded in a setuid, setgid or file-capability
             * process; otherwise no dma-buf fd on this path (helper V2 pixel mode),
             * or no usable render node. Point at that, not at the build. */
            if (getauxval(AT_SECURE)) {
                drmtap_set_error(ctx,
                    "scanout modifier 0x%lx needs a GPU detile, and GL is not loaded "
                    "in a setuid, setgid or file-capability process: grab through the "
                    "helper from an unprivileged process instead",
                    (unsigned long)modifier);
                return -ENOTSUP;
            }
            drmtap_set_error(ctx,
                "scanout modifier 0x%lx needs a GPU detile, and the EGL detile "
                "this build carries was unavailable or failed: no dma-buf fd on "
                "this path, no usable render node, or the import itself failed. "
                "Run with debug logging on -- the EGL: lines say which",
                (unsigned long)modifier);
#else
            /* The single most common cause of this error, and previously
             * indistinguishable from the runtime one. `meson setup build` alone
             * leaves the egl feature on 'auto', which silently builds the stub
             * when the headers are missing, and nothing says so until a real
             * tiled scanout arrives -- which on modern Intel is every frame. */
            drmtap_set_error(ctx,
                "scanout modifier 0x%lx needs a GPU detile, and THIS BUILD HAS "
                "NO EGL BACKEND. Install the EGL development packages "
                "(Debian/Ubuntu: libegl-dev libgles2-mesa-dev) and reconfigure "
                "with -Degl=enabled, which fails the build instead of silently "
                "producing this stub", (unsigned long)modifier);
#endif
            return -ENOTSUP;
        }
        if (ret == 0) {
            frame->data = out;
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
                        out, out,
                        frame->width, frame->height,
                        frame->stride, frame->stride,
                        frame->format, ctx->cur_hdr_max_nits);
                } else {
                    drmtap_debug_log(ctx,
                        "auto-process: SDR 10-bit AR30 -> 8-bit XRGB8888");
                    conv = drmtap_convert_format(
                        out, out,
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

    /* Real tiling modifier (non-LINEAR, non-INVALID -- a genuinely tiled scanout;
     * linear/unknown-layout buffers already returned above) on a driver we have no
     * CPU deswizzle for, and EGL did not detile it (unavailable, or it failed).
     * Returning the raw tiled mapping relabelled linear would hand the caller
     * corruption reported as valid -- the same failure the CCS branch now guards.
     * Fail closed instead, symmetric with it, so the caller ends the stream and
     * falls back (do_grab propagates this; e.g. rustdesk demotes to PipeWire). */
    drmtap_debug_log(ctx, "auto-process: unknown driver '%s' mod=0x%lx cannot "
                     "deswizzle and EGL unavailable -- failing closed",
                     driver, (unsigned long)modifier);
#ifdef HAVE_EGL
    drmtap_set_error(ctx,
        "tiled scanout (driver '%s', modifier 0x%lx) has no CPU deswizzle here, "
        "and the EGL detile this build carries was unavailable or failed. Run "
        "with debug logging on -- the EGL: lines say which",
        driver, (unsigned long)modifier);
#else
    drmtap_set_error(ctx,
        "tiled scanout (driver '%s', modifier 0x%lx) has no CPU deswizzle, and "
        "THIS BUILD HAS NO EGL BACKEND. Install the EGL development packages "
        "(Debian/Ubuntu: libegl-dev libgles2-mesa-dev) and reconfigure with "
        "-Degl=enabled", driver, (unsigned long)modifier);
#endif
    return -ENOTSUP;
}

/* ========================================================================= */
/* Public API                                                                */
/* ========================================================================= */

static int drmtap_grab_impl(drmtap_ctx *ctx, drmtap_frame_info *frame);
/* Public entry: see drmtap_error_ensure (every error return leaves a message). */
int drmtap_grab(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    unsigned mark = drmtap_error_mark(ctx);
    return drmtap_error_ensure(ctx, mark, drmtap_grab_impl(ctx, frame), "drmtap_grab");
}
static int drmtap_grab_impl(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    if (!ctx || !frame) {
        return -EINVAL;
    }
    return do_grab(ctx, frame, 0, 0);  /* zero-copy: DMA-BUF fd only */
}

static int drmtap_grab_desc_impl(drmtap_ctx *ctx, drmtap_dmabuf_desc *desc,
                     drmtap_frame_info *frame);
/* Public entry: see drmtap_error_ensure (every error return leaves a message). */
int drmtap_grab_desc(drmtap_ctx *ctx, drmtap_dmabuf_desc *desc,
                     drmtap_frame_info *frame) {
    unsigned mark = drmtap_error_mark(ctx);
    return drmtap_error_ensure(ctx, mark, drmtap_grab_desc_impl(ctx, desc, frame), "drmtap_grab_desc");
}
static int drmtap_grab_desc_impl(drmtap_ctx *ctx, drmtap_dmabuf_desc *desc,
                     drmtap_frame_info *frame) {
    if (!ctx || !desc || !frame) {
        return -EINVAL;
    }
    /* do_grab names the plane a frame came from; a frame this call refuses
     * below never reaches the caller, so the name must not move either. */
    uint32_t plane_before = ctx->grab_plane_id;
    int ret = do_grab(ctx, frame, 0, 1);  /* zero-copy: DMA-BUF fd + metadata */
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
        ctx->grab_plane_id = plane_before;
        return -ENOTSUP;
    }
    /* Snapshot the full descriptor. The plane layout and HDR state are cached
     * on ctx during do_grab (from GetFB2 and the connector metadata) — they are
     * NOT in frame_info, which is exactly why a split exporter needs this call
     * rather than drmtap_grab alone. */
    /* A descriptor never pairs the plane layout of one framebuffer with the
     * buffer of another (the helper path once sent the window buffer under the
     * covering layer's layout during an underlay). */
    if (frame->fb_id != ctx->fb2_fb_id) {
        drmtap_set_error(ctx, "grab_desc: the buffer is fb %u but the plane layout is "
                         "of fb %u; frame skipped", frame->fb_id, ctx->fb2_fb_id);
        drmtap_frame_release(ctx, frame);
        ctx->grab_plane_id = plane_before;
        return -EAGAIN;
    }
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

static int drmtap_grab_mapped_impl(drmtap_ctx *ctx, drmtap_frame_info *frame);
/* Public entry: see drmtap_error_ensure (every error return leaves a message). */
int drmtap_grab_mapped(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    unsigned mark = drmtap_error_mark(ctx);
    return drmtap_error_ensure(ctx, mark, drmtap_grab_mapped_impl(ctx, frame), "drmtap_grab_mapped");
}
static int drmtap_grab_mapped_impl(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    if (!ctx || !frame) {
        return -EINVAL;
    }
    return do_grab(ctx, frame, 1, 0);  /* mapped: mmap'd pixel data */
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

int drmtap_frame_owns_data(const drmtap_frame_info *frame) {
    if (!frame || !frame->data) {
        return 0;
    }
    const frame_priv_t *priv = (const frame_priv_t *)frame->_priv;
    return priv && priv->mapped && frame->data == priv->mapped;
}

/* ========================================================================= */
/* Fast persistent-mmap capture (double-buffer cache)                         */
/* ========================================================================= */

// Clean up all cached buffer slots
void drmtap_fast_cleanup(drmtap_ctx *ctx) {
    /* Before the fds go away: a slot may still owe a SYNC_END, and it must be
     * issued while its prime_fd is open. */
    fast_sync_close(ctx);
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
    ctx->fast_no_cpu_map = 0;
    /* fast_no_privilege is deliberately NOT cleared: it records a property of the
     * PROCESS (no CAP_SYS_ADMIN), not of the plane, the framebuffer or the device,
     * so re-discovering it after every teardown would repeat the whole GetFB2 dance
     * to reach the same verdict. */
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
    // Close its sync window first: after the munmap and close below the fd is
    // gone and the END can no longer be issued. The one caller already closes
    // every window before getting here, so this is a backstop for the next one.
    if (ctx->fast_slots[0].sync_started && ctx->fast_slots[0].prime_fd >= 0) {
        dmabuf_sync_end(ctx->fast_slots[0].prime_fd);
        ctx->fast_slots[0].sync_started = 0;
    }
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
    ctx->fb2_fb_id = ctx->fast_slots[slot].fb_id;
    for (int p = 0; p < 4; p++) {
        ctx->fb2_pitches[p] = ctx->fast_slots[slot].fb2_pitches[p];
        ctx->fb2_offsets[p] = ctx->fast_slots[slot].fb2_offsets[p];
    }
}

static int drmtap_grab_mapped_fast_impl(drmtap_ctx *ctx, drmtap_frame_info *frame);
/* Public entry: see drmtap_error_ensure (every error return leaves a message). */
int drmtap_grab_mapped_fast(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    unsigned mark = drmtap_error_mark(ctx);
    return drmtap_error_ensure(ctx, mark, drmtap_grab_mapped_fast_impl(ctx, frame), "drmtap_grab_mapped_fast");
}
static int drmtap_grab_mapped_fast_impl(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    if (!ctx || !frame) {
        return -EINVAL;
    }
    if (ctx->is_render_only) {
        drmtap_set_error(ctx, "context is render-only (drmtap_open_render); "
                         "grab needs a KMS context from drmtap_open");
        return -ENOTSUP;
    }

    /* Established on an earlier call that this process cannot export the scanout and
     * that this entry point has no helper fallback. Answer immediately and quietly:
     * the caller has the reason in drmtap_error() from the first failure, and a
     * capture loop calling this at frame rate must not produce a line per frame. */
    ctx->sw_override_on = 0;   /* only a covering-layer frame narrows from another rect */
    int stand_in = 0;          /* the layer a fallback captures (nl > 1) */
    if (ctx->fast_no_privilege) {
        drmtap_set_error(ctx,
            "drmtap_grab_mapped_fast needs CAP_SYS_ADMIN in this process and has no "
            "helper fallback; use drmtap_grab_mapped()");
        return -EACCES;
    }

    int ret;

    /* Step 1 (0.6.0): the lit planes, one OBJ_GETPROPERTIES per plane that can
     * drive the CRTC. Several: compose them (nothing is cached for a composed
     * frame; the EGL import cache keeps the imports). One: the cached
     * single-buffer path below, with the fb_id from this same read, unless it is
     * stretched or smaller than the screen (composed, 0.6.1).
     * No plane properties (no atomic cap): the legacy lookup, as before. */
    drmtap_layer_set ls;
    int nl = read_layers(ctx, &ls);
    if (nl > 1) {
        if (!ctx->fast_initialized) {
            read_hdr_for_current_crtc(ctx);
        }
        /* The previous fast frame is superseded: close its CPU-access window. */
        fast_sync_close(ctx);
        int r = grab_composed(ctx, frame, 0);
        if (r == 0) {
            return 0;
        }
        if (r == -EACCES) {
            ctx->fast_no_privilege = 1;
            drmtap_set_error(ctx,
                "drmtap_grab_mapped_fast needs CAP_SYS_ADMIN in this process and has no "
                "helper fallback; use drmtap_grab_mapped()");
            return -EACCES;
        }
        if (!layer_fallback_ok(r)) {
            return r;
        }
        log_layer_fallback(ctx, nl, r);
        /* One plane alone stands for the screen: never the window of an underlay. */
        uint32_t cw = 0, ch = 0;
        stand_in = drmtap_kms_crtc_size(ctx->drm_fd, ctx->crtc_id, &cw, &ch) == 0
                   ? drmtap_layers_stand_in(&ls, cw, ch) : -1;
        if (stand_in < 0) {
            drmtap_set_error(ctx, "crtc %u: %d planes could not be composed (%s) and no "
                             "single plane covers the screen: no frame", ctx->crtc_id, nl,
                             strerror(-r));
            return r;
        }
    }
    if (nl == 1) {
        /* 0.6.1: one plane stretched to the screen, or smaller than it, is composed
         * to the CRTC size as in drmtap_grab_mapped(); any error falls back to the
         * cached single-buffer path below (no helper here, as for nl > 1). */
        uint32_t cw = 0, ch = 0;
        if (drmtap_kms_crtc_size(ctx->drm_fd, ctx->crtc_id, &cw, &ch) == 0 &&
            !drmtap_layer_is_screen(&ls.layers[0], cw, ch) &&
            one_layer_should_compose(ctx, &ls.layers[0], cw, ch)) {
            if (!ctx->fast_initialized) {
                read_hdr_for_current_crtc(ctx);
            }
            fast_sync_close(ctx);
            int r = grab_composed(ctx, frame, 0);
            one_layer_composed(ctx, &ls.layers[0], cw, ch, r);
            if (r == 0) {
                return 0;
            }
            if (r == -EACCES) {
                ctx->fast_no_privilege = 1;
                drmtap_set_error(ctx,
                    "drmtap_grab_mapped_fast needs CAP_SYS_ADMIN in this process and has no "
                    "helper fallback; use drmtap_grab_mapped()");
                return -EACCES;
            }
            log_one_layer_fallback(ctx, r);
        }
    }
    uint32_t fb_id = 0;
    if (nl >= 1) {
        int pick = 0;
        if (nl > 1) {
            int pi = drmtap_layers_primary(&ls);
            pick = stand_in;
            if (pick != pi) {
                /* the covering layer: narrow the padded width from its own rect */
                drmtap_layer_plane_rect(&ls.layers[pick], &ctx->sw_override);
                ctx->sw_override_on = 1;
            }
        }
        if (!ctx->fast_initialized) {
            read_hdr_for_current_crtc(ctx);
            for (int i = 0; i < DRMTAP_FAST_SLOTS; i++) {
                memset(&ctx->fast_slots[i], 0, sizeof(ctx->fast_slots[i]));
                ctx->fast_slots[i].prime_fd = -1;
            }
            ctx->fast_last_fb_id = 0;
            ctx->fast_no_cpu_map = 0;
            ctx->fast_initialized = 1;
            drmtap_debug_log(ctx, "fast2: initialized plane=%u", ls.layers[pick].plane_id);
        }
        /* The slots are keyed by fb_id, so a different plane needs no slot reset,
         * but the not-CPU-mappable verdict belongs to the old plane: drop it. */
        if (ctx->fast_plane_id != ls.layers[pick].plane_id) {
            ctx->fast_no_cpu_map = 0;
        }
        ctx->fast_plane_id = ls.layers[pick].plane_id;
        fb_id = ls.layers[pick].fb_id;
    } else if (nl == 0) {
        drmtap_set_error(ctx, "fast2: no plane is scanning out on crtc %u (display "
                         "asleep or a modeset in flight)", ctx->crtc_id);
        if (ctx->fast_initialized) {
            drmtap_fast_cleanup(ctx);
        }
        return -ENODEV;
    } else {
        /* Step 1: Find plane on first call only */
        if (!ctx->fast_initialized) {
            ctx->fast_plane_id = find_primary_plane(ctx);
            read_hdr_for_current_crtc(ctx);
            if (ctx->fast_plane_id == 0) {
                drmtap_set_error(ctx, "No active plane found for capture");
                return -ENODEV;
            }
            for (int i = 0; i < DRMTAP_FAST_SLOTS; i++) {
                memset(&ctx->fast_slots[i], 0, sizeof(ctx->fast_slots[i]));
                ctx->fast_slots[i].prime_fd = -1;
            }
            ctx->fast_last_fb_id = 0;
            ctx->fast_no_cpu_map = 0;
            ctx->fast_initialized = 1;
            drmtap_debug_log(ctx, "fast2: initialized plane=%u", ctx->fast_plane_id);
        }

        /* Step 2: Refresh fb_id (cheap ioctl, ~0.05ms) */
        drmModePlane *plane = drmModeGetPlane(ctx->drm_fd, ctx->fast_plane_id);
        if (!plane || plane->fb_id == 0) {
            drmtap_set_error(ctx, plane
                ? "fast2: plane %u has no framebuffer bound (display asleep or a modeset in flight)"
                : "fast2: plane %u disappeared (a modeset changed the plane layout)",
                ctx->fast_plane_id);
            if (plane) drmModeFreePlane(plane);
            drmtap_fast_cleanup(ctx);
            return -ENODEV;
        }
        fb_id = plane->fb_id;
        drmModeFreePlane(plane);
    }

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
                            ctx->fast_slots[i].fb_width,
                            ctx->fast_slots[i].height);
                    if (ret < 0) return ret;
                    fast_sync_close(ctx);
                } else {
                    fast_sync_begin(ctx, i);
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
                ctx->grab_plane_id = ctx->fast_plane_id; /* a frame came from it */
                ctx->last_frame_composed = 0;
                return 0;   /* always return as new frame */
            }
        }
        /* Slot not found — fall through to acquire */
    }

    /* Step 4: fb_id changed — check if we have this buffer cached */
    /* Before find_or_alloc_slot, which EVICTS slot 0 when the cache is full:
     * eviction closes prime_fd and memsets the slot, so an open sync window
     * would lose both its fd and its flag and the END would never be issued.
     * This grab supersedes the previous frame anyway, so the window is finished
     * with by now. */
    fast_sync_close(ctx);
    int slot = find_or_alloc_slot(ctx, fb_id);

    if (ctx->fast_slots[slot].fb_id == fb_id && ctx->fast_slots[slot].mmap_ptr) {
        /* ═══ CACHE HIT ═══ Buffer already mapped from a previous flip!
         * Skip: GetFB2, PrimeHandleToFD, mmap  (saves ~4ms)
         * Only do: TRANSFER_FROM_HOST (the unavoidable part) */
        drmtap_debug_log(ctx, "fast2: CACHE HIT fb=%u slot=%d gem=%u",
                         fb_id, slot, ctx->fast_slots[slot].gem_handle);

        if (is_virtio_gpu(ctx)) {
            ret = virtio_transfer_from_host(ctx, ctx->fast_slots[slot].gem_handle,
                                             ctx->fast_slots[slot].fb_width,
                                             ctx->fast_slots[slot].height);
            if (ret < 0) {
                drmtap_debug_log(ctx, "fast2: cached transfer failed: %d", ret);
                return ret;
            }
            fast_sync_close(ctx);
        } else {
            fast_sync_begin(ctx, slot);
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

        ctx->grab_plane_id = ctx->fast_plane_id; /* a frame came from it */
        ctx->last_frame_composed = 0;

        return 0;   /* new frame */
    }

    /* ═══ CACHE MISS ═══ First time seeing this fb_id — full setup.
     * On a device whose scanout cannot be CPU-mapped this is EVERY frame, and
     * it is not a cold start: the EGL fd path below has no CPU mapping to cache,
     * so there is never anything to hit. Say which one it is, or the log reads
     * like a cache that is broken rather than one that does not apply. */
    drmtap_debug_log(ctx, "fast2: CACHE MISS fb=%u slot=%d (%s)", fb_id, slot,
                     ctx->fast_no_cpu_map ? "uncached: scanout is not CPU-mappable"
                                          : "cold start");

    drmModeFB2 *fb2 = drmModeGetFB2(ctx->drm_fd, fb_id);
    if (!fb2) {
        ret = -errno;
        drmtap_set_error(ctx, "fast2: drmModeGetFB2(%u) failed: %s",
                         fb_id, strerror(errno));
        return ret;
    }

    close_auxiliary_gem_handles(ctx, fb2);

    if (fb2->handles[0] == 0) {
        /* drmModeGetFB2 zeroes the GEM handles for a caller without CAP_SYS_ADMIN.
         * drmtap_grab_mapped treats that as "go through the helper"; this entry point
         * has no such branch, so it cannot serve an unprivileged caller on ANY gpu.
         * Say that, name the call that does work, and latch it so the next frame is
         * answered at the top of the function instead of repeating this whole dance.
         * Reported as endless cache-miss lines with the cause buried, which sent both
         * the reporter and me after a tiling bug that was not there (issue #36). */
        drmModeFreeFB2(fb2);
        ctx->fast_no_privilege = 1;
        /* Nothing on this path can be served again, so release whatever the slots hold
         * (prime fds, mappings, GEM handles) now instead of at drmtap_close. Normally
         * there is nothing: an unprivileged process never populated a slot in the first
         * place. It matters for the one case that can, a process that captured with
         * CAP_SYS_ADMIN and then dropped it, where the slots are live and unusable.
         * Runs AFTER the flag is set, since cleanup deliberately leaves it alone. */
        drmtap_fast_cleanup(ctx);
        drmtap_debug_log(ctx,
            "fast2: this process has no CAP_SYS_ADMIN, and the fast path has no helper "
            "fallback (it caches a CPU mapping per framebuffer; a helper hands over a "
            "fresh fd per grab, so there is nothing to cache). Use drmtap_grab_mapped(), "
            "which falls back to the helper. Not repeated per frame.");
        drmtap_set_error(ctx,
            "drmtap_grab_mapped_fast needs CAP_SYS_ADMIN in this process and has no "
            "helper fallback; use drmtap_grab_mapped()");
        return -EACCES;
    }

    /* Bound the DIMENSIONS too, not just stride*height: width is unconstrained by
     * validate_fb_size, and every converted output is sized width*height*4. */
    ret = drmtap_validate_fb_dims(fb2->width, fb2->height);
    if (ret == 0) {
        ret = validate_fb_size(fb2->pitches[0], fb2->height);
    }
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
    ctx->fb2_fb_id = fb_id;
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
        drmtap_set_error(ctx, "fast2: exporting fb=%u as a DMA-BUF failed: %s",
                         fb_id, strerror(errno));
        drmtap_gem_close(ctx, fb2->handles[0]);
        drmModeFreeFB2(fb2);
        return ret;
    }

    /* mmap */
    size_t size = (size_t)fb2->pitches[0] * fb2->height;
    void *mapped = MAP_FAILED;
    // Whether the START below succeeded. It is taken before the slot is stored,
    // so it is carried here and recorded when it is.
    int sync_started = 0;

    if (is_virtio_gpu(ctx)) {
        ret = virtio_transfer_from_host(ctx, fb2->handles[0],
                                         fb2->width, fb2->height);
        if (ret >= 0) {
            mapped = virtio_dumb_mmap(ctx, fb2->handles[0], size);
        }
    }

    /* Skip the mmap once this context has learned it cannot work here: it is a
     * property of the driver and the buffer placement, not of one frame, so
     * retrying it per frame only costs a failing syscall. */
    if (mapped == MAP_FAILED && !ctx->fast_no_cpu_map) {
        /* Unlike do_grab, this START is load-bearing: it is the ONLY cache
         * invalidation before the first read of the slot being populated, since
         * no second START follows a successful mmap here. It must stay, and it
         * must be recorded so an END follows exactly once. */
        sync_started = (dmabuf_sync_start(prime_fd) == 0);
        mapped = mmap(NULL, size, PROT_READ, MAP_SHARED,
                      prime_fd, fb2->offsets[0]);
        /* Test hook (drmtap_force_mmap_fail): drop a successful mapping so the
         * EGL fd fallback is exercised on a GPU whose scanout IS CPU-mappable.
         * Inside this block on purpose, so a forced failure takes exactly the
         * same path as a real one, sticky flag and log line included. */
        if (mapped != MAP_FAILED && drmtap_force_mmap_fail()) {
            munmap(mapped, size);
            mapped = MAP_FAILED;
            errno = ENOTSUP;
        }
        if (mapped == MAP_FAILED) {
            /* Log the reason ONCE, at the transition. Without this the fast path
             * looks identical to a working one that simply never hits, which is
             * exactly how it was reported: pages of "CACHE MISS ... cold start"
             * and no explanation. */
            drmtap_debug_log(ctx,
                "fast2: scanout fb=%u is not CPU-mappable (%s); serving every frame "
                "through the EGL fd path, no slot is cached",
                fb_id, strerror(errno));
            ctx->fast_no_cpu_map = 1;
        }
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
            frame->width = drmtap_scanout_width(ctx, fb2->width,
                                                fb2_effective_modifier(fb2));
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
            /* gpu_auto_process EGL-read the fd back into the conversion destination
             * (the caller's output buffer if one was set, otherwise the ctx-owned
             * one), so the fd and handle are no longer needed and nothing is cached
             * for fb_id. */
            /* No slot is cached for this fb, so nothing will ever pair the
             * START taken above. Close the window on the fd while it is still
             * open, rather than leaving the exporter with an access that only
             * the fd going away ends. */
            /* No reset of sync_started after the END: this path returns without
             * caching a slot, so nothing reads it again. */
            if (sync_started) {
                dmabuf_sync_end(prime_fd);
            }
            close(prime_fd);
            frame->dma_buf_fd = -1;  /* prime_fd is closed; don't hand back a stale fd */
            drmtap_gem_close(ctx, fb2->handles[0]);
            drmModeFreeFB2(fb2);
            ctx->fast_last_fb_id = 0;  /* uncached: next call does a fresh setup */
            if (pr != 0) {
                return pr;
            }
            ctx->grab_plane_id = ctx->fast_plane_id; /* a frame came from it */
            ctx->last_frame_composed = 0;
            return 0;
        }
#endif
        /* Neither a CPU mapping nor EGL: there is no way to read this scanout.
         * Say so through drmtap_error, not only the debug log -- a caller that
         * sees the failure otherwise has nothing to go on, which is how this
         * arrived as a bug report full of log lines instead of a message. */
        drmtap_set_error(ctx,
            "fast2: scanout fb=%u cannot be CPU-mapped and no EGL backend is "
            "available (built without egl/glesv2, or no usable render node). "
            "This GPU needs the EGL detile path; use drmtap_grab_mapped() or "
            "build with -Degl=enabled.", fb_id);
        if (sync_started) {
            dmabuf_sync_end(prime_fd);
        }
        close(prime_fd);
        drmtap_gem_close(ctx, fb2->handles[0]);
        drmModeFreeFB2(fb2);
        return -ENOMEM;
    }

    /* Store in slot */
    ctx->fast_slots[slot].fb_id = fb_id;
    ctx->fast_slots[slot].gem_handle = fb2->handles[0];
    ctx->fast_slots[slot].prime_fd = prime_fd;
    /* Hand the open window to the slot, so the next grab (or fast_cleanup)
     * issues its END. */
    ctx->fast_slots[slot].sync_started = sync_started;
    ctx->fast_slots[slot].mmap_ptr = mapped;
    ctx->fast_slots[slot].mmap_size = size;
    /* Cache the SCANNED-OUT width, so every later cache hit replays the narrowed
     * geometry without paying another plane query on the hot path, and keep the
     * framebuffer width beside it for anything that describes the BUFFER rather than
     * the visible image (a virtio transfer box covers the buffer being made coherent,
     * not the visible sub-region of it). The mmap size below deliberately stays on
     * fb2->pitches[0] * fb2->height: the mapping is of the whole padded framebuffer. */
    ctx->fast_slots[slot].fb_width = fb2->width;
    ctx->fast_slots[slot].width =
        drmtap_scanout_width(ctx, fb2->width, fb2_effective_modifier(fb2));
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

    ctx->grab_plane_id = ctx->fast_plane_id; /* a frame came from it */
    ctx->last_frame_composed = 0;

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

static int drmtap_convert_dmabuf_impl(drmtap_ctx *ctx, const drmtap_dmabuf_desc *desc,
                          drmtap_frame_info *frame);
/* Public entry: see drmtap_error_ensure (every error return leaves a message). */
int drmtap_convert_dmabuf(drmtap_ctx *ctx, const drmtap_dmabuf_desc *desc,
                          drmtap_frame_info *frame) {
    unsigned mark = drmtap_error_mark(ctx);
    return drmtap_error_ensure(ctx, mark, drmtap_convert_dmabuf_impl(ctx, desc, frame), "drmtap_convert_dmabuf");
}
static int drmtap_convert_dmabuf_impl(drmtap_ctx *ctx, const drmtap_dmabuf_desc *desc,
                          drmtap_frame_info *frame) {
    if (!ctx || !desc || !frame) {
        return -EINVAL;
    }
    /* One buffer as it is: what plane_rotation answered for an earlier composed or
     * packet frame no longer applies (convert_packet sets it again after this). */
    ctx->last_frame_composed = 0;

    uint32_t num_planes = desc->num_planes ? desc->num_planes : 1;
    if (num_planes > 4 || desc->width == 0) {
        drmtap_set_error(ctx, "convert: invalid descriptor "
                         "(width=%u num_planes=%u)",
                         desc->width, desc->num_planes);
        return -EINVAL;
    }
    /* Bound the DIMENSIONS too (see the GetFB2 entry points): desc comes over IPC. */
    int ret = drmtap_validate_fb_dims(desc->width, desc->height);
    if (ret == 0) {
        ret = validate_fb_size(desc->pitches[0], desc->height);
    }
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
                         "(fourcc %s)", desc->pitches[0], desc->width,
                         drmtap_fourcc_str(desc->format, (char[11]){0}));
        return -EINVAL;
    }

    /* Stage the plane + HDR metadata exactly where the in-process grab
     * (drmModeGetFB2 + the connector HDR read) would put it — the conversion
     * paths below consume both from the context. */
    ctx->fb2_num_planes = (int)num_planes;
    ctx->fb2_fb_id = desc->fb_id;
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
        /* Planes 1..n are the CCS / clear-colour auxiliaries of a compressed
         * scanout (Gen12+), and they used to reach eglCreateImage with no bound
         * at all. Their height is a format-specific fraction of the image height
         * -- a Gen12 CCS plane is 1/16th -- not desc->height, so their full
         * extent is not computable here and a pitches[p]*height bound would
         * REJECT legitimate compressed scanouts. Bound what is knowable instead:
         * the offset plus one row must lie inside the buffer. Weaker than the
         * plane-0 check by necessity, but it can never reject a valid descriptor
         * and it does reject the wild values that were forwarded unchecked. */
        for (uint32_t p = 1; p < num_planes; p++) {
            if ((uint64_t)desc->offsets[p] + (uint64_t)desc->pitches[p]
                    > (uint64_t)fd_size) {
                drmtap_set_error(ctx, "convert: plane %u exceeds dma-buf size "
                                 "(fd_size=%lld, offset %u + pitch %u)",
                                 p, (long long)fd_size, desc->offsets[p],
                                 desc->pitches[p]);
                return -EINVAL;
            }
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
            frame->data = egl_data;   /* the resolved destination: caller's output
                                       * buffer if set, else the ctx-owned one */
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
        void *out = NULL;
        ret = drmtap_ensure_out(ctx, out_size, &out);
        if (ret == 0) {
            const uint8_t *src = mapped;
            uint8_t *dst = out;
            if (desc->pitches[0] == out_stride) {
                memcpy(dst, src, out_size);
            } else {
                for (uint32_t y = 0; y < desc->height; y++) {
                    memcpy(dst + (size_t)y * out_stride,
                           src + (size_t)y * desc->pitches[0], out_stride);
                }
            }
            frame->data = out;
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

/* ========================================================================= */
/* Split capture of a multi-plane scanout: the packet (0.6.0)                */
/* ========================================================================= */

void drmtap_packet_release(drmtap_packet *pkt) {
    if (!pkt) {
        return;
    }
    uint32_t n = pkt->nfds < DRMTAP_PACKET_MAX_FDS ? pkt->nfds : DRMTAP_PACKET_MAX_FDS;
    for (uint32_t i = 0; i < n; i++) {
        if (pkt->fds[i] >= 0) {
            close(pkt->fds[i]);
        }
    }
    memset(pkt, 0, sizeof(*pkt));
    for (int i = 0; i < DRMTAP_PACKET_MAX_FDS; i++) {
        pkt->fds[i] = -1;
    }
}

static int packet_fill(drmtap_ctx *ctx, drmtap_packet *pkt, drmtap_layer_set *s) {
    int len = drmtap_packet_encode(s, pkt->bytes, sizeof(pkt->bytes));
    if (len < 0) {
        drmtap_layers_close_fds(s);
        drmtap_set_error(ctx, "packet: the layer set does not encode (%d)", len);
        return len;
    }
    pkt->len = (uint32_t)len;
    pkt->nfds = s->nfds;
    for (uint32_t i = 0; i < s->nfds; i++) {
        pkt->fds[i] = s->fds[i];
        s->fds[i] = -1;
    }
    s->nfds = 0;
    int pi = drmtap_layers_primary(s);
    ctx->grab_plane_id = s->layers[pi >= 0 ? pi : 0].plane_id;
    /* The converter composes a packet of several layers, or of one layer that is not
     * the screen as it is: that frame is in CRTC orientation. One layer that is the
     * screen converts as its buffer, the plane rotation applies. */
    ctx->last_frame_composed = (s->count > 1) ||
        !drmtap_layer_is_screen(&s->layers[0], s->crtc_w, s->crtc_h);
    return 0;
}

/* A one-layer packet built from drmtap_grab_desc(), for the cases that cannot see
 * the layers: no plane properties (no atomic cap), or a helper older than 0.6.0. It
 * converts exactly like that descriptor would. */
static int packet_from_desc(drmtap_ctx *ctx, drmtap_packet *pkt) {
    drmtap_dmabuf_desc d;
    drmtap_frame_info f;
    int r = drmtap_grab_desc(ctx, &d, &f);
    if (r != 0) {
        return r;
    }
    drmtap_layer_set s;
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < DRMTAP_LAYERS_MAX_FDS; i++) {
        s.fds[i] = -1;
    }
    int fd = fcntl(f.dma_buf_fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) {
        r = errno ? -errno : -EIO;
        drmtap_frame_release(ctx, &f);
        return r;
    }
    drmtap_frame_release(ctx, &f);
    /* The plane's real rotation, so the converter can report it (grab_desc just
     * named the plane; a driver without the property is ROTATE_0). */
    uint32_t rot = DRMTAP_ROT_0;
    if (drmtap_plane_rotation(ctx, &rot) != 0 || !drmtap_rotation_valid(rot)) {
        rot = DRMTAP_ROT_0;
    }
    drmtap_layer *l = &s.layers[0];
    s.count = 1;
    s.lit = 1;
    s.nfds = 1;
    s.fds[0] = fd;
    s.crtc_w = d.width;
    s.crtc_h = d.height;
    s.hdr_eotf = d.hdr_eotf;
    s.hdr_max_nits = d.hdr_max_nits;
    l->plane_id = ctx->grab_plane_id;
    l->fb_id = d.fb_id;
    l->fb_width = d.width;
    l->fb_height = d.height;
    l->format = d.format;
    l->modifier = d.modifier;
    l->num_planes = (d.num_planes >= 1 && d.num_planes <= 4) ? d.num_planes : 1;
    for (uint32_t p = 0; p < 4; p++) {
        int used = p < l->num_planes;
        l->fd_index[p] = used ? 0 : DRMTAP_PKT_NO_FD;
        l->offsets[p] = used ? d.offsets[p] : 0;
        l->pitches[p] = used ? d.pitches[p] : 0;
    }
    l->src_w = d.width << 16;
    l->src_h = d.height << 16;
    l->crtc_w = d.width;
    l->crtc_h = d.height;
    l->alpha = 0xffffu;
    l->blend = DRMTAP_BLEND_PREMULTI;
    l->rotation = rot;
    l->flags = DRMTAP_LAYER_FLAG_PRIMARY;
    return packet_fill(ctx, pkt, &s);
}

static int drmtap_grab_packet_impl(drmtap_ctx *ctx, drmtap_packet *pkt);
/* Public entry: see drmtap_error_ensure (every error return leaves a message). */
int drmtap_grab_packet(drmtap_ctx *ctx, drmtap_packet *pkt) {
    unsigned mark = drmtap_error_mark(ctx);
    return drmtap_error_ensure(ctx, mark, drmtap_grab_packet_impl(ctx, pkt), "drmtap_grab_packet");
}
static int drmtap_grab_packet_impl(drmtap_ctx *ctx, drmtap_packet *pkt) {
    /* Empty the packet before ANY return, NULL ctx included: the documented
     * cleanup is drmtap_packet_release() even after a failure, which would
     * otherwise close whatever stack garbage the fds held. */
    if (pkt) {
        memset(pkt, 0, sizeof(*pkt));
        for (int i = 0; i < DRMTAP_PACKET_MAX_FDS; i++) {
            pkt->fds[i] = -1;
        }
    }
    if (!ctx || !pkt) {
        return -EINVAL;
    }
    if (ctx->is_render_only) {
        drmtap_set_error(ctx, "context is render-only (drmtap_open_render); "
                         "grab needs a KMS context from drmtap_open");
        return -ENOTSUP;
    }
    if (ctx->crtc_id == 0 && select_first_active_crtc(ctx) == 0) {
        drmtap_set_error(ctx, "No active plane found for capture");
        return -ENODEV;
    }
    read_hdr_for_current_crtc(ctx);

    drmtap_layer_set s;
    int retried = 0;
    int n = drmtap_kms_capture_layers(ctx->drm_fd, &ctx->layer_cache, ctx->crtc_id, &s,
                                      &retried);
    if (n >= 0) {
        ctx->last_layer_count = (uint32_t)n;
    }
    if (n == -ENOTSUP) {
        return packet_from_desc(ctx, pkt);
    }
    if (n == -EACCES) {
        int r = helper_layers(ctx, &s);
        if (r == -ENOTSUP || r == -EIO) {
            /* An old helper, or one that could not export these layers (more than a
             * packet carries, for one): one plane, like grab_desc. */
            return packet_from_desc(ctx, pkt);
        }
        if (r < 0) {
            return r;
        }
        return packet_fill(ctx, pkt, &s);
    }
    if (n < 0) {
        drmtap_set_error(ctx, "reading the planes of crtc %u: %s", ctx->crtc_id,
                         strerror(-n));
        return n;
    }
    if (n == 0) {
        drmtap_set_error(ctx, "No active plane found for capture");
        return -ENODEV;
    }
    if (s.lit > s.count) {
        if (!ctx->logged_layer_overflow) {
            ctx->logged_layer_overflow = 1;
            drmtap_debug_log(ctx, "crtc %u has %u lit planes, more than the %d a packet "
                             "carries: sending one plane alone (the primary, or the plane "
                             "that covers the screen when the primary is a window under "
                             "it). Said once per context", ctx->crtc_id, s.lit,
                             DRMTAP_MAX_LAYERS);
        }
        uint32_t cw = 0, ch = 0;
        int si = drmtap_kms_crtc_size(ctx->drm_fd, ctx->crtc_id, &cw, &ch) == 0
                 ? drmtap_layers_stand_in(&s, cw, ch) : -1;
        if (si < 0) {
            drmtap_set_error(ctx, "crtc %u has %u lit planes, more than a packet carries, "
                             "and none of the kept ones covers the screen: no frame",
                             ctx->crtc_id, s.lit);
            return -E2BIG;
        }
        s.layers[0] = s.layers[si];
        s.count = 1;
        s.lit = 1;
        int r = drmtap_kms_export_layers(ctx->drm_fd, &s);
        if (r == -EACCES) {
            return packet_from_desc(ctx, pkt);
        }
        if (r < 0) {
            drmtap_set_error(ctx, "exporting plane %u: %s", s.layers[0].plane_id, strerror(-r));
            return r;
        }
    }
    if (retried) {
        drmtap_debug_log(ctx, "layers: a plane flipped during the export; read them again");
    }
    int r = drmtap_kms_crtc_size(ctx->drm_fd, ctx->crtc_id, &s.crtc_w, &s.crtc_h);
    if (r < 0) {
        drmtap_layers_close_fds(&s);
        drmtap_set_error(ctx, "crtc %u has no mode", ctx->crtc_id);
        return r;
    }
    s.hdr_eotf = ctx->cur_hdr_eotf;
    s.hdr_max_nits = ctx->cur_hdr_max_nits;
    return packet_fill(ctx, pkt, &s);
}

/* One layer through the single-buffer convert, exactly as drmtap_grab_desc() +
 * drmtap_convert_dmabuf() would handle that buffer, including the padded-scanout
 * width: the buffer as it is, unrotated, at its own size. A layer whose planes live
 * in several buffer objects cannot be one descriptor, and a YUV or 16-bit layer
 * (less than 4 bytes per pixel) is not one the descriptor path reads, even with
 * all its planes in one fd (the usual NV12 layout); the GPU composer reads them,
 * with the same result: the w x h buffer as it is (rotation 0, opaque, no scaling,
 * no placement), so the caller's rotation answer applies to it the same way. */
static int convert_one_layer(drmtap_ctx *ctx, const drmtap_layer_set *s, uint32_t i,
                             drmtap_frame_info *frame) {
    const drmtap_layer *l = &s->layers[i];
    uint32_t w = l->fb_width;
    if (s->crtc_w > 0 && s->crtc_w < l->fb_width) {
        drmtap_plane_rect rect;
        memset(&rect, 0, sizeof(rect));
        drmtap_layer_plane_rect(l, &rect);
        int linear = (l->modifier == DRM_FORMAT_MOD_LINEAR ||
                      l->modifier == DRM_FORMAT_MOD_INVALID);
        w = drmtap_scanout_width_of(l->fb_width, &rect, linear, NULL);
    }
    /* Below 4 bytes per pixel, or YUV at 4 (AYUV, XYUV8888): the descriptor path
     * carries no YCbCr encoding or range, the composer does. */
    int gpu = layer_min_bpp(l->format) < 4 || l->format == DRM_FORMAT_AYUV ||
              l->format == DRM_FORMAT_XYUV8888;
    for (uint32_t p = 1; !gpu && p < l->num_planes; p++) {
        gpu = l->fd_index[p] != l->fd_index[0];
    }
    if (gpu) {
        drmtap_layer_set one = *s;
        drmtap_layer *o = &one.layers[0];
        *o = *l;
        one.count = 1;
        one.lit = 1;
        one.crtc_w = w;
        one.crtc_h = l->fb_height;
        o->src_x = 0;
        o->src_y = 0;
        o->src_w = w << 16;
        o->src_h = l->fb_height << 16;
        o->crtc_x = 0;
        o->crtc_y = 0;
        o->crtc_w = w;
        o->crtc_h = l->fb_height;
        o->rotation = DRMTAP_ROT_0;
        o->alpha = 0xffffu;
        o->blend = DRMTAP_BLEND_NONE;
        return compose_layers(ctx, &one, frame);
    }
    drmtap_dmabuf_desc d;
    memset(&d, 0, sizeof(d));
    d.dma_buf_fd = s->fds[l->fd_index[0]];
    d.width = w;
    d.height = l->fb_height;
    d.format = l->format;
    d.modifier = l->modifier;
    d.fb_id = l->fb_id;
    d.num_planes = l->num_planes;
    for (uint32_t p = 0; p < l->num_planes && p < 4; p++) {
        d.offsets[p] = l->offsets[p];
        d.pitches[p] = l->pitches[p];
    }
    d.hdr_eotf = s->hdr_eotf;
    d.hdr_max_nits = s->hdr_max_nits;
    return drmtap_convert_dmabuf(ctx, &d, frame);
}

static int drmtap_convert_packet_impl(drmtap_ctx *ctx, const void *bytes, size_t len,
                          const int *fds, size_t nfds, drmtap_frame_info *frame);
/* Public entry: see drmtap_error_ensure (every error return leaves a message). */
int drmtap_convert_packet(drmtap_ctx *ctx, const void *bytes, size_t len,
                          const int *fds, size_t nfds, drmtap_frame_info *frame) {
    unsigned mark = drmtap_error_mark(ctx);
    return drmtap_error_ensure(ctx, mark,
                               drmtap_convert_packet_impl(ctx, bytes, len, fds, nfds, frame), "drmtap_convert_packet");
}
static int drmtap_convert_packet_impl(drmtap_ctx *ctx, const void *bytes, size_t len,
                          const int *fds, size_t nfds, drmtap_frame_info *frame) {
    if (!ctx || !bytes || !frame || (nfds > 0 && !fds)) {
        return -EINVAL;
    }
    if (len > DRMTAP_PACKET_MAX_BYTES || nfds > DRMTAP_PACKET_MAX_FDS) {
        drmtap_set_error(ctx, "convert_packet: %zu bytes / %zu fds is more than a packet "
                         "holds", len, nfds);
        return -EINVAL;
    }
    drmtap_layer_set s;
    int r = drmtap_packet_decode((const uint8_t *)bytes, len, nfds, &s);
    if (r < 0) {
        drmtap_set_error(ctx, "convert_packet: %s",
            r == -EPROTO ? "not a libdrmtap packet" :
            r == -EPROTONOSUPPORT ? "a packet version this library does not read "
                                    "(update libdrmtap on the converting side)" :
            "malformed packet");
        return r;
    }
    for (size_t i = 0; i < nfds; i++) {
        s.fds[i] = fds[i];
    }
    r = validate_layer_fds(ctx, &s);
    if (r < 0) {
        return r;
    }
    ctx->cur_hdr_eotf = s.hdr_eotf;
    ctx->cur_hdr_max_nits = s.hdr_max_nits;
    ctx->last_frame_composed = 0;
    if (s.count == 1 && (drmtap_layer_is_screen(&s.layers[0], s.crtc_w, s.crtc_h) ||
                         !one_layer_should_compose(ctx, &s.layers[0], s.crtc_w, s.crtc_h))) {
        r = convert_one_layer(ctx, &s, 0, frame);
        if (r == 0) {
            ctx->packet_rotation = s.layers[0].rotation;
            ctx->last_frame_composed = DRMTAP_FRAME_ONE_LAYER;
        }
        return r;
    }
    r = compose_layers(ctx, &s, frame);
    if (r == 0) {
        ctx->last_frame_composed = 1;
    }
    if (s.count == 1) {
        one_layer_composed(ctx, &s.layers[0], s.crtc_w, s.crtc_h, r);
    }
    if (r == 0 || (s.count > 1 && !layer_fallback_ok(r))) {
        return r;
    }
    /* One layer that could not be composed to the screen, whatever the error: its
     * buffer, as before 0.6.1. */
    if (s.count == 1) {
        log_one_layer_fallback(ctx, r);
    } else {
        log_layer_fallback(ctx, (int)s.count, r);
    }
    int si = s.count == 1 ? 0 : drmtap_layers_stand_in(&s, s.crtc_w, s.crtc_h);
    if (si < 0) {
        drmtap_set_error(ctx, "convert_packet: %u layers could not be composed (%s) and no "
                         "single layer covers the CRTC: no frame", s.count, strerror(-r));
        return r;
    }
    uint32_t one = (uint32_t)si;
    r = convert_one_layer(ctx, &s, one, frame);
    if (r == 0) {
        ctx->packet_rotation = s.layers[one].rotation;
        ctx->last_frame_composed = DRMTAP_FRAME_ONE_LAYER;
    }
    return r;
}
