/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file layers_kms.h
 * @brief Read the lit planes of a CRTC into a drmtap_layer_set and export their
 *        framebuffers. Shared by the library (drm_grab.c) and the privileged
 *        helper, header-only for the same reason as layers.h.
 *
 * Cost model. Property ids are looked up by name ONCE per device (a sweep of
 * every plane and every property), then each frame costs one
 * DRM_IOCTL_MODE_OBJ_GETPROPERTIES per non-cursor plane that can drive the CRTC
 * (five on an i915 pipe), with fixed stack buffers and no allocation. Measured on
 * the old planecost probe at about 15 us for all 24 planes of the NUC.
 *
 * The plane properties used here (FB_ID, CRTC_ID, SRC_*, CRTC_*) are atomic
 * properties: a client sees them only after DRM_CLIENT_CAP_ATOMIC. The caller sets
 * the cap; without it the cache build reports -ENOTSUP and the caller keeps the
 * legacy primary-plane path.
 */

#ifndef DRMTAP_LAYERS_KMS_H
#define DRMTAP_LAYERS_KMS_H

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>      /* O_CLOEXEC, behind DRM_CLOEXEC */
#include <unistd.h>
#include <sys/ioctl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "layers.h"


static inline void drmtap_kms_enum_add(uint32_t *n, uint64_t *vals, uint32_t *ours,
                                       uint64_t kval, uint32_t our) {
    if (*n < DRMTAP_KMS_ENUMS) {
        vals[*n] = kval;
        ours[*n] = our;
        (*n)++;
    }
}

/* Map a kernel enum value to ours; `dflt` for a value the table does not hold. */
static inline uint32_t drmtap_kms_enum_map(uint32_t n, const uint64_t *vals,
                                           const uint32_t *ours, uint64_t kval,
                                           uint32_t dflt) {
    for (uint32_t i = 0; i < n; i++) {
        if (vals[i] == kval) {
            return ours[i];
        }
    }
    return dflt;
}

/* Look up every property id once, for the non-cursor planes (the capture never
 * reads a cursor plane). Returns 0, or -ENOTSUP when the planes do not show the
 * atomic properties (no DRM_CLIENT_CAP_ATOMIC, or a driver without atomic) or when
 * the device has more non-cursor planes than `max`: both send the caller to the
 * single-plane legacy lookup, as before 0.6.0, instead of a cache that misses the
 * primary plane. Another -errno otherwise. The verdict is kept in c->state. */
static inline int drmtap_kms_cache_build_max(int fd, drmtap_kms_cache *c, uint32_t max) {
    memset(c, 0, sizeof(*c));
    c->crtc_index = -1;
    if (max > DRMTAP_KMS_MAX_PLANES) {
        max = DRMTAP_KMS_MAX_PLANES;
    }
    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    if (!pr) {
        int err = errno ? errno : EIO;
        return -err;
    }
    int usable = 1;
    int overflow = 0;
    drmtap_kms_plane tmp;
    for (uint32_t i = 0; i < pr->count_planes; i++) {
        drmtap_kms_plane *kp = &tmp;
        memset(kp, 0, sizeof(*kp));
        kp->id = pr->planes[i];
        kp->type = DRM_PLANE_TYPE_OVERLAY;
        drmModePlane *pl = drmModeGetPlane(fd, kp->id);
        if (pl) {
            kp->possible_crtcs = pl->possible_crtcs;
            drmModeFreePlane(pl);
        }
        drmModeObjectProperties *props =
            drmModeObjectGetProperties(fd, kp->id, DRM_MODE_OBJECT_PLANE);
        if (!props) {
            continue;
        }
        for (uint32_t p = 0; p < props->count_props; p++) {
            drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[p]);
            if (!prop) {
                continue;
            }
            const char *n = prop->name;
            uint32_t id = prop->prop_id;
            if (!strcmp(n, "type")) {
                kp->type = (uint32_t)props->prop_values[p];
            } else if (!strcmp(n, "FB_ID")) {
                kp->p_fb = id;
            } else if (!strcmp(n, "CRTC_ID")) {
                kp->p_crtc = id;
            } else if (!strcmp(n, "SRC_X")) {
                kp->p_sx = id;
            } else if (!strcmp(n, "SRC_Y")) {
                kp->p_sy = id;
            } else if (!strcmp(n, "SRC_W")) {
                kp->p_sw = id;
            } else if (!strcmp(n, "SRC_H")) {
                kp->p_sh = id;
            } else if (!strcmp(n, "CRTC_X")) {
                kp->p_cx = id;
            } else if (!strcmp(n, "CRTC_Y")) {
                kp->p_cy = id;
            } else if (!strcmp(n, "CRTC_W")) {
                kp->p_cw = id;
            } else if (!strcmp(n, "CRTC_H")) {
                kp->p_ch = id;
            } else if (!strcmp(n, "zpos")) {
                kp->p_zpos = id;
            } else if (!strcmp(n, "alpha")) {
                kp->p_alpha = id;
            } else if (!strcmp(n, "rotation")) {
                kp->p_rot = id;
            } else if (!strcmp(n, "pixel blend mode")) {
                kp->p_blend = id;
                for (int e = 0; e < prop->count_enums; e++) {
                    const char *en = prop->enums[e].name;
                    uint64_t v = prop->enums[e].value;
                    if (!strcmp(en, "Pre-multiplied")) {
                        drmtap_kms_enum_add(&kp->nblend, kp->blend_val, kp->blend_our, v,
                                            DRMTAP_BLEND_PREMULTI);
                    } else if (!strcmp(en, "Coverage")) {
                        drmtap_kms_enum_add(&kp->nblend, kp->blend_val, kp->blend_our, v,
                                            DRMTAP_BLEND_COVERAGE);
                    } else if (!strcmp(en, "None")) {
                        drmtap_kms_enum_add(&kp->nblend, kp->blend_val, kp->blend_our, v,
                                            DRMTAP_BLEND_NONE);
                    }
                }
            } else if (!strcmp(n, "COLOR_ENCODING")) {
                kp->p_enc = id;
                for (int e = 0; e < prop->count_enums; e++) {
                    const char *en = prop->enums[e].name;
                    uint64_t v = prop->enums[e].value;
                    uint32_t our = strstr(en, "2020") ? DRMTAP_YCBCR_BT2020
                                 : strstr(en, "709") ? DRMTAP_YCBCR_BT709
                                 : DRMTAP_YCBCR_BT601;
                    drmtap_kms_enum_add(&kp->nenc, kp->enc_val, kp->enc_our, v, our);
                }
            } else if (!strcmp(n, "COLOR_RANGE")) {
                kp->p_range = id;
                for (int e = 0; e < prop->count_enums; e++) {
                    const char *en = prop->enums[e].name;
                    uint64_t v = prop->enums[e].value;
                    uint32_t our = strstr(en, "full") ? DRMTAP_RANGE_FULL
                                                      : DRMTAP_RANGE_LIMITED;
                    drmtap_kms_enum_add(&kp->nrange, kp->range_val, kp->range_our, v, our);
                }
            }
            drmModeFreeProperty(prop);
        }
        drmModeFreeObjectProperties(props);
        if (kp->type == DRM_PLANE_TYPE_CURSOR) {
            continue;
        }
        if (c->count >= max) {
            overflow = 1;
            break;
        }
        if (!kp->p_fb || !kp->p_crtc || !kp->p_sx || !kp->p_sy || !kp->p_sw ||
            !kp->p_sh || !kp->p_cx || !kp->p_cy || !kp->p_cw || !kp->p_ch) {
            usable = 0;
        }
        c->planes[c->count++] = *kp;
    }
    drmModeFreePlaneResources(pr);
    if (overflow) {
        c->count = 0;
        usable = 0;
    }
    c->state = usable ? 1 : -1;
    return usable ? 0 : -ENOTSUP;
}
static inline int drmtap_kms_cache_build(int fd, drmtap_kms_cache *c) {
    return drmtap_kms_cache_build_max(fd, c, DRMTAP_KMS_MAX_PLANES);
}

/* Position of crtc_id in the resources' CRTC list: the bit possible_crtcs uses. */
static inline int drmtap_kms_crtc_index(int fd, uint32_t crtc_id) {
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        return -1;
    }
    int idx = -1;
    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == crtc_id) {
            idx = i;
            break;
        }
    }
    drmModeFreeResources(res);
    return idx;
}

/* One OBJ_GETPROPERTIES into fixed buffers. Returns the property count (which can
 * exceed the buffer, then only the first DRMTAP_KMS_MAX_PROPS are valid), or
 * -errno. The kernel writes both arrays through the u64 pointers below, which is
 * why they are not const (cppcheck cannot see through the cast). */
static inline int drmtap_kms_plane_values(int fd, uint32_t plane_id,
                                          // cppcheck-suppress constParameter
                                          uint32_t ids[DRMTAP_KMS_MAX_PROPS],
                                          // cppcheck-suppress constParameter
                                          uint64_t vals[DRMTAP_KMS_MAX_PROPS]) {
    struct drm_mode_obj_get_properties a;
    memset(&a, 0, sizeof(a));
    a.props_ptr = (uint64_t)(uintptr_t)ids;
    a.prop_values_ptr = (uint64_t)(uintptr_t)vals;
    a.count_props = DRMTAP_KMS_MAX_PROPS;
    a.obj_id = plane_id;
    a.obj_type = DRM_MODE_OBJECT_PLANE;
    if (drmIoctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &a) != 0) {
        int err = errno ? errno : EIO;
        return -err;
    }
    return (int)a.count_props;
}

static inline int drmtap_kms_value(uint32_t n, const uint32_t *ids, const uint64_t *vals,
                                   uint32_t id, uint64_t *out) {
    if (id == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (ids[i] == id) {
            *out = vals[i];
            return 1;
        }
    }
    return 0;
}

/* Fill `s` with the lit non-cursor planes of crtc_id, bottom to top, without
 * touching any framebuffer (fb_width/height/format/planes stay 0 until export).
 * Returns the number of lit planes (s->lit, which may exceed s->count when more
 * than DRMTAP_MAX_LAYERS are lit; the primary is always among the kept ones),
 * -ENOTSUP when the atomic plane properties are not visible, or another -errno. */
static inline int drmtap_kms_read_layers(int fd, drmtap_kms_cache *c, uint32_t crtc_id,
                                         drmtap_layer_set *s) {
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < DRMTAP_LAYERS_MAX_FDS; i++) {
        s->fds[i] = -1;
    }
    s->crtc_id = crtc_id;
    if (c->state == 0) {
        int r = drmtap_kms_cache_build(fd, c);
        if (r < 0 && r != -ENOTSUP) {
            c->state = 0;   /* a failed read is not a verdict: look again next time */
            return r;
        }
    }
    if (c->state < 0) {
        return -ENOTSUP;
    }
    if (c->crtc_id != crtc_id || c->crtc_index < 0) {
        c->crtc_id = crtc_id;
        c->crtc_index = drmtap_kms_crtc_index(fd, crtc_id);
    }
    uint32_t ids[DRMTAP_KMS_MAX_PROPS];
    uint64_t vals[DRMTAP_KMS_MAX_PROPS];
    for (uint32_t i = 0; i < c->count; i++) {
        const drmtap_kms_plane *kp = &c->planes[i];
        if (kp->type == DRM_PLANE_TYPE_CURSOR) {
            continue;
        }
        if (c->crtc_index >= 0 && c->crtc_index < 32 &&
            !(kp->possible_crtcs & (1u << c->crtc_index))) {
            continue;
        }
        int n = drmtap_kms_plane_values(fd, kp->id, ids, vals);
        if (n < 0) {
            return n;
        }
        uint32_t nn = (uint32_t)n < DRMTAP_KMS_MAX_PROPS ? (uint32_t)n : DRMTAP_KMS_MAX_PROPS;
        uint64_t v = 0, fb = 0, crtc = 0;
        if (!drmtap_kms_value(nn, ids, vals, kp->p_crtc, &crtc) || crtc != crtc_id ||
            !drmtap_kms_value(nn, ids, vals, kp->p_fb, &fb) || fb == 0) {
            continue;
        }
        s->lit++;
        int primary = (kp->type == DRM_PLANE_TYPE_PRIMARY);
        drmtap_layer *l;
        if (s->count < DRMTAP_MAX_LAYERS) {
            l = &s->layers[s->count++];
        } else if (primary) {
            l = &s->layers[DRMTAP_MAX_LAYERS - 1];   /* keep the primary, see above */
        } else {
            continue;
        }
        memset(l, 0, sizeof(*l));
        l->plane_id = kp->id;
        l->fb_id = (uint32_t)fb;
        l->flags = primary ? DRMTAP_LAYER_FLAG_PRIMARY : 0;
        if (drmtap_kms_value(nn, ids, vals, kp->p_sx, &v)) l->src_x = (uint32_t)v;
        if (drmtap_kms_value(nn, ids, vals, kp->p_sy, &v)) l->src_y = (uint32_t)v;
        if (drmtap_kms_value(nn, ids, vals, kp->p_sw, &v)) l->src_w = (uint32_t)v;
        if (drmtap_kms_value(nn, ids, vals, kp->p_sh, &v)) l->src_h = (uint32_t)v;
        if (drmtap_kms_value(nn, ids, vals, kp->p_cx, &v)) l->crtc_x = (int32_t)(int64_t)v;
        if (drmtap_kms_value(nn, ids, vals, kp->p_cy, &v)) l->crtc_y = (int32_t)(int64_t)v;
        if (drmtap_kms_value(nn, ids, vals, kp->p_cw, &v)) l->crtc_w = (uint32_t)v;
        if (drmtap_kms_value(nn, ids, vals, kp->p_ch, &v)) l->crtc_h = (uint32_t)v;
        /* No zpos property: the primary is at the bottom, overlays above it in plane
         * order (what the kernel's normalized zpos does for such a driver). */
        l->zpos = primary ? 0u : i + 1u;
        if (drmtap_kms_value(nn, ids, vals, kp->p_zpos, &v)) l->zpos = (uint32_t)v;
        l->alpha = 0xffffu;
        if (drmtap_kms_value(nn, ids, vals, kp->p_alpha, &v)) l->alpha = (uint32_t)(v & 0xffffu);
        l->blend = DRMTAP_BLEND_PREMULTI;
        if (drmtap_kms_value(nn, ids, vals, kp->p_blend, &v)) {
            l->blend = drmtap_kms_enum_map(kp->nblend, kp->blend_val, kp->blend_our, v,
                                           DRMTAP_BLEND_PREMULTI);
        }
        l->rotation = DRMTAP_ROT_0;
        if (drmtap_kms_value(nn, ids, vals, kp->p_rot, &v) &&
            drmtap_rotation_valid((uint32_t)v)) {
            l->rotation = (uint32_t)v;
        }
        l->color_encoding = DRMTAP_YCBCR_BT601;
        if (drmtap_kms_value(nn, ids, vals, kp->p_enc, &v)) {
            l->color_encoding = drmtap_kms_enum_map(kp->nenc, kp->enc_val, kp->enc_our, v,
                                                    DRMTAP_YCBCR_BT601);
        }
        l->color_range = DRMTAP_RANGE_LIMITED;
        if (drmtap_kms_value(nn, ids, vals, kp->p_range, &v)) {
            l->color_range = drmtap_kms_enum_map(kp->nrange, kp->range_val, kp->range_our,
                                                 v, DRMTAP_RANGE_LIMITED);
        }
    }
    drmtap_layers_sort(s->layers, s->count);
    return (int)s->lit;
}

/* The CRTC's mode size. -ENODATA when it has no mode. */
static inline int drmtap_kms_crtc_size(int fd, uint32_t crtc_id, uint32_t *w, uint32_t *h) {
    drmModeCrtc *crtc = drmModeGetCrtc(fd, crtc_id);
    if (!crtc) {
        int err = errno ? errno : EIO;
        return -err;
    }
    int ok = crtc->mode_valid && crtc->mode.hdisplay > 0 && crtc->mode.vdisplay > 0;
    *w = crtc->mode.hdisplay;
    *h = crtc->mode.vdisplay;
    drmModeFreeCrtc(crtc);
    return ok ? 0 : -ENODATA;
}

static inline void drmtap_layers_close_fds(drmtap_layer_set *s) {
    for (uint32_t i = 0; i < s->nfds && i < DRMTAP_LAYERS_MAX_FDS; i++) {
        if (s->fds[i] >= 0) {
            close(s->fds[i]);
        }
        s->fds[i] = -1;
    }
    s->nfds = 0;
}

static inline void drmtap_kms_gem_close(int fd, uint32_t handle) {
    if (handle) {
        struct drm_gem_close gc;
        memset(&gc, 0, sizeof(gc));
        gc.handle = handle;
        drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
    }
}

/* GetFB2 + one read-only PRIME export per distinct buffer object of every layer.
 * Fills the framebuffer fields and s->fds. Every GEM handle GetFB2 minted is
 * closed before returning (each plane may report its own). -EACCES when GetFB2
 * hides the handles (no CAP_SYS_ADMIN): the caller goes through the helper. On
 * any failure no fd is left open. */
static inline int drmtap_kms_export_layers(int fd, drmtap_layer_set *s) {
    int ret = 0;
    s->nfds = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        drmtap_layer *l = &s->layers[i];
        drmModeFB2 *fb2 = drmModeGetFB2(fd, l->fb_id);
        if (!fb2) {
            ret = errno ? -errno : -EIO;
            goto fail;
        }
        if (fb2->handles[0] == 0) {
            drmModeFreeFB2(fb2);
            ret = -EACCES;
            goto fail;
        }
        l->fb_width = fb2->width;
        l->fb_height = fb2->height;
        l->format = fb2->pixel_format;
        l->modifier = (fb2->flags & DRM_MODE_FB_MODIFIERS) ? fb2->modifier
                                                           : DRM_FORMAT_MOD_INVALID;
        uint32_t np = 0;
        for (uint32_t p = 0; p < 4; p++) {
            if (fb2->handles[p] || fb2->pitches[p]) {
                np = p + 1;
            }
        }
        l->num_planes = np ? np : 1;
        uint32_t seen_h[4] = {0, 0, 0, 0};
        uint8_t seen_i[4] = {0, 0, 0, 0};
        uint32_t nseen = 0;
        for (uint32_t p = 0; p < 4; p++) {
            l->fd_index[p] = DRMTAP_PKT_NO_FD;
            l->offsets[p] = 0;
            l->pitches[p] = 0;
            if (p >= l->num_planes || ret != 0) {
                continue;
            }
            uint32_t h = fb2->handles[p] ? fb2->handles[p] : fb2->handles[0];
            int found = -1;
            for (uint32_t k = 0; k < nseen; k++) {
                if (seen_h[k] == h) {
                    found = (int)k;
                    break;
                }
            }
            if (found < 0) {
                int pfd = -1;
                if (s->nfds >= DRMTAP_LAYERS_MAX_FDS) {
                    ret = -E2BIG;
                    continue;
                }
                if (drmPrimeHandleToFD(fd, h, DRM_CLOEXEC, &pfd) != 0 || pfd < 0) {
                    ret = errno ? -errno : -EIO;
                    continue;
                }
                s->fds[s->nfds] = pfd;
                seen_h[nseen] = h;
                seen_i[nseen] = (uint8_t)s->nfds;
                found = (int)nseen;
                nseen++;
                s->nfds++;
            }
            l->fd_index[p] = seen_i[found];
            l->offsets[p] = fb2->offsets[p];
            l->pitches[p] = fb2->pitches[p];
        }
        /* Close every distinct handle GetFB2 handed us, exported or not. */
        for (uint32_t p = 0; p < 4; p++) {
            uint32_t h = fb2->handles[p];
            int dup = 0;
            for (uint32_t q = 0; q < p; q++) {
                if (fb2->handles[q] == h) {
                    dup = 1;
                }
            }
            if (h && !dup) {
                drmtap_kms_gem_close(fd, h);
            }
        }
        drmModeFreeFB2(fb2);
        if (ret != 0) {
            goto fail;
        }
        if (!drmtap_layer_valid(l, s->nfds)) {
            ret = -EINVAL;
            goto fail;
        }
    }
    return 0;
fail:
    drmtap_layers_close_fds(s);
    return ret;
}

/* Read, export, read again. When the compositor flipped a layer (or moved one)
 * between the two reads the exported buffers may not belong to one frame, so the
 * whole thing runs once more and that second result is used as it is. An export
 * that fails with -ENOENT (an fb removed between the read and GetFB2) also gets
 * that one more pass. Returns
 * the lit count with s->fds filled (when s->lit <= DRMTAP_MAX_LAYERS), 0 when
 * nothing is lit, or -errno. *retried (optional) is set when the second pass ran. */
static inline int drmtap_kms_capture_layers(int fd, drmtap_kms_cache *c, uint32_t crtc_id,
                                            drmtap_layer_set *s, int *retried) {
    if (retried) {
        *retried = 0;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        int n = drmtap_kms_read_layers(fd, c, crtc_id, s);
        if (n <= 0 || s->lit > s->count) {
            return n;
        }
        int r = drmtap_kms_export_layers(fd, s);
        if (r == -ENOENT && attempt == 0) {
            /* A framebuffer went away between the read and GetFB2 (a window closed
             * or resized): read the planes again rather than fail the frame. */
            if (retried) {
                *retried = 1;
            }
            continue;
        }
        if (r < 0) {
            return r;
        }
        if (attempt == 1) {
            return n;
        }
        drmtap_layer_set again;
        int m = drmtap_kms_read_layers(fd, c, crtc_id, &again);
        if (m == n && drmtap_layers_same(s, &again)) {
            return n;
        }
        drmtap_layers_close_fds(s);
        if (retried) {
            *retried = 1;
        }
    }
    return -EAGAIN;   /* not reached */
}

#endif /* DRMTAP_LAYERS_KMS_H */
