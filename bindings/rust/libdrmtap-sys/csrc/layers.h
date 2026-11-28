/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file layers.h
 * @brief A scanout frame as a list of KMS planes ("layers"), and the libdrmtap
 *        packet that carries such a list between two processes.
 *
 * Pure code only: no DRM ioctl, no allocation, no libdrm. It is header-only
 * because the privileged helper (helper/drmtap-helper.c, its own executable)
 * must encode the same packet the library decodes, and a header shared by both
 * builds cannot drift (the same reason wire.h is header-only).
 *
 * Why layers: a compositor may scan out a window on its own plane (KWin 6.8 does
 * it on i915/xe when one output is enabled, rustdesk/rustdesk#17187). The image on
 * the panel is then the hardware blend of several framebuffers, and reading the
 * PRIMARY plane alone returns only part of it.
 */

#ifndef DRMTAP_LAYERS_H
#define DRMTAP_LAYERS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

/* At most this many layers in a frame and in a packet. i915 has five non-cursor
 * planes per pipe and KWin was seen using two; a frame with more lit planes than
 * this is captured from its primary plane only (see the callers). */
#define DRMTAP_MAX_LAYERS 4
/* One fd per distinct buffer object of each layer: at most 4 planes per layer. */
#define DRMTAP_LAYERS_MAX_FDS 16

/* Pixel blend modes, our own numbering (the kernel exposes them as an enum
 * property whose values are matched by name when the property is first read). */
#define DRMTAP_BLEND_PREMULTI 0u  /* "Pre-multiplied": out = src + dst * (1 - a) */
#define DRMTAP_BLEND_COVERAGE 1u  /* "Coverage": out = src * a + dst * (1 - a) */
#define DRMTAP_BLEND_NONE     2u  /* "None": pixel alpha ignored */

/* YCbCr encoding and range of a YUV layer (ignored for RGB). */
#define DRMTAP_YCBCR_BT601  0u
#define DRMTAP_YCBCR_BT709  1u
#define DRMTAP_YCBCR_BT2020 2u
#define DRMTAP_RANGE_LIMITED 0u
#define DRMTAP_RANGE_FULL    1u

/* DRM rotation bits (drm_mode.h values, repeated so this header needs no libdrm). */
#define DRMTAP_ROT_0        0x01u
#define DRMTAP_ROT_90       0x02u
#define DRMTAP_ROT_180      0x04u
#define DRMTAP_ROT_270      0x08u
#define DRMTAP_ROT_MASK     0x0fu
#define DRMTAP_REFLECT_X    0x10u
#define DRMTAP_REFLECT_Y    0x20u

#define DRMTAP_LAYER_FLAG_PRIMARY 0x1u

/* Bounds a layer field must satisfy. Far above real hardware; they exist so no
 * product of two of them can wrap. */
#define DRMTAP_LAYER_MAX_DIM 32768u

typedef struct {
    uint32_t plane_id;
    uint32_t fb_id;
    uint32_t fb_width, fb_height;   /* the framebuffer object */
    uint32_t format;                /* DRM fourcc */
    uint64_t modifier;              /* DRM_FORMAT_MOD_INVALID when the fb has none */
    uint32_t num_planes;            /* 1..4 */
    uint32_t offsets[4];
    uint32_t pitches[4];
    uint8_t  fd_index[4];           /* per plane, index into the set's fd array */
    uint32_t src_x, src_y, src_w, src_h;  /* 16.16 fixed point, in the fb */
    int32_t  crtc_x, crtc_y;        /* may be negative: the plane hangs off the edge */
    uint32_t crtc_w, crtc_h;
    uint32_t zpos;                  /* normalized order, bottom = smallest */
    uint32_t alpha;                 /* plane alpha, 0..0xffff */
    uint32_t blend;                 /* DRMTAP_BLEND_* */
    uint32_t rotation;              /* DRMTAP_ROT_* | DRMTAP_REFLECT_* */
    uint32_t color_encoding;        /* DRMTAP_YCBCR_* */
    uint32_t color_range;           /* DRMTAP_RANGE_* */
    uint32_t flags;                 /* DRMTAP_LAYER_FLAG_* */
} drmtap_layer;

typedef struct {
    uint32_t crtc_id;
    uint32_t crtc_w, crtc_h;        /* the CRTC mode: the size of the composed frame */
    uint32_t count;                 /* layers[] used, sorted bottom to top */
    drmtap_layer layers[DRMTAP_MAX_LAYERS];
    uint32_t lit;                   /* lit non-cursor planes seen, may exceed count */
    uint32_t hdr_eotf, hdr_max_nits;
    uint32_t nfds;
    int      fds[DRMTAP_LAYERS_MAX_FDS];
} drmtap_layer_set;

/* Per-device cache of plane property ids, filled once by drmtap_kms_cache_build()
 * (layers_kms.h). Declared here, with no libdrm type in it, so a context can
 * embed it without its users needing the libdrm headers. */
#define DRMTAP_KMS_MAX_PLANES 32
#define DRMTAP_KMS_MAX_PROPS 64
#define DRMTAP_KMS_ENUMS 4

typedef struct {
    uint32_t id;
    uint32_t possible_crtcs;
    uint32_t type;                  /* DRM_PLANE_TYPE_* */
    uint32_t p_fb, p_crtc;
    uint32_t p_sx, p_sy, p_sw, p_sh;
    uint32_t p_cx, p_cy, p_cw, p_ch;
    uint32_t p_zpos, p_alpha, p_blend, p_rot, p_enc, p_range;  /* 0 = absent */
    /* Enum properties: the kernel value of each of our names. */
    uint32_t nblend, nenc, nrange;
    uint64_t blend_val[DRMTAP_KMS_ENUMS];
    uint32_t blend_our[DRMTAP_KMS_ENUMS];
    uint64_t enc_val[DRMTAP_KMS_ENUMS];
    uint32_t enc_our[DRMTAP_KMS_ENUMS];
    uint64_t range_val[DRMTAP_KMS_ENUMS];
    uint32_t range_our[DRMTAP_KMS_ENUMS];
} drmtap_kms_plane;

typedef struct {
    int state;                      /* 0 = not built, 1 = built, -1 = unusable */
    uint32_t count;
    drmtap_kms_plane planes[DRMTAP_KMS_MAX_PLANES];
    uint32_t crtc_id;               /* the CRTC crtc_index belongs to */
    int crtc_index;                 /* bit in possible_crtcs, -1 = unknown */
} drmtap_kms_cache;

/* ========================================================================= */
/* Ordering and geometry                                                     */
/* ========================================================================= */

/* Bottom to top, the way the hardware blends: by zpos, ties by plane id (the
 * kernel normalizes equal zpos by plane index, which follows the id). Insertion
 * sort: at most DRMTAP_MAX_LAYERS entries. */
static inline void drmtap_layers_sort(drmtap_layer *l, uint32_t n) {
    for (uint32_t i = 1; i < n; i++) {
        drmtap_layer t = l[i];
        uint32_t j = i;
        while (j > 0 && (l[j - 1].zpos > t.zpos ||
                         (l[j - 1].zpos == t.zpos && l[j - 1].plane_id > t.plane_id))) {
            l[j] = l[j - 1];
            j--;
        }
        l[j] = t;
    }
}

/* 1 when the layer's CRTC rectangle covers the whole w x h CRTC. */
static inline int drmtap_layer_covers(const drmtap_layer *l, uint32_t w, uint32_t h) {
    return l->crtc_x <= 0 && l->crtc_y <= 0 &&
           (int64_t)l->crtc_x + (int64_t)l->crtc_w >= (int64_t)w &&
           (int64_t)l->crtc_y + (int64_t)l->crtc_h >= (int64_t)h;
}

/* Whether one layer alone IS the w x h screen as its buffer: its CRTC rectangle is
 * exactly the CRTC and it is not scaled (its source rectangle has that size). Only such
 * a layer can be returned as its buffer; any other one (stretched, or smaller than
 * the screen) has to be composed to w x h, or the frame comes out the size of the
 * buffer (a 40x40 plane stretched to the screen). A 90/270 plane read from KMS has
 * its source transposed against its CRTC rectangle; the one-layer packet built from
 * a descriptor (packet_from_desc, also in 0.6.0 exporters) gives its CRTC rectangle
 * the source size at any rotation, and matches the first test. */
static inline int drmtap_layer_is_screen(const drmtap_layer *l, uint32_t w, uint32_t h) {
    /* exactly the CRTC: a plane hanging off an edge (larger than the screen) is not */
    if (l->crtc_x != 0 || l->crtc_y != 0 || l->crtc_w != w || l->crtc_h != h) {
        return 0;
    }
    uint32_t sw = l->src_w >> 16, sh = l->src_h >> 16;
    uint32_t rot = l->rotation & DRMTAP_ROT_MASK;
    int turned = rot == DRMTAP_ROT_90 || rot == DRMTAP_ROT_270;
    return (sw == l->crtc_w && sh == l->crtc_h) ||
           (turned && sh == l->crtc_w && sw == l->crtc_h);
}

/* A fourcc for a message: its four characters when all are printable ASCII, else
 * 0x%08x. The value can come from a hostile packet, so no control byte (ESC, CR)
 * reaches drmtap_error() or a log. `buf` needs 11 bytes; returns it. */
static inline const char *drmtap_fourcc_str(uint32_t f, char *buf) {
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)(f >> (8 * i));
        ok = ok && c >= 0x20 && c < 0x7f;
    }
    if (ok) {
        for (int i = 0; i < 4; i++) {
            buf[i] = (char)(f >> (8 * i));
        }
        buf[4] = 0;
    } else {
        static const char hx[] = "0123456789abcdef";
        buf[0] = '0';
        buf[1] = 'x';
        for (int i = 0; i < 8; i++) {
            buf[2 + i] = hx[(f >> (28 - 4 * i)) & 0xf];
        }
        buf[10] = 0;
    }
    return buf;
}

/* Index of the PRIMARY plane's layer, or -1. */
static inline int drmtap_layers_primary(const drmtap_layer_set *s) {
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->layers[i].flags & DRMTAP_LAYER_FLAG_PRIMARY) {
            return (int)i;
        }
    }
    return -1;
}

/* The one layer that stands for the screen when the layers cannot be composed:
 * the primary when it covers the w x h CRTC; otherwise the topmost layer that
 * covers it (in an underlay the primary is the window, and the covering layer is
 * the desktop with the window's hole, the grab_desc rule); -1 when no layer covers
 * the CRTC, so no single buffer is the screen. */
static inline int drmtap_layers_stand_in(const drmtap_layer_set *s, uint32_t w, uint32_t h) {
    int pi = drmtap_layers_primary(s);
    if (pi >= 0 && drmtap_layer_covers(&s->layers[pi], w, h)) {
        return pi;
    }
    for (int i = (int)s->count - 1; i >= 0; i--) {
        if (drmtap_layer_covers(&s->layers[i], w, h)) {
            return i;
        }
    }
    return -1;
}

/* The shape of one layer on a w x h CRTC, without its framebuffer: what decides
 * whether it can be composed apart from its format (0.6.1, see one_nocompose_key). */
static inline uint64_t drmtap_layer_shape_key(const drmtap_layer *l, uint32_t crtc,
                                              uint32_t w, uint32_t h) {
    uint64_t k = 0xcbf29ce484222325ull;
    const uint32_t v[13] = { crtc, l->plane_id, l->src_x, l->src_y, l->src_w, l->src_h,
                             (uint32_t)l->crtc_x, (uint32_t)l->crtc_y, l->crtc_w, l->crtc_h,
                             l->rotation, w, h };
    for (int i = 0; i < 13; i++) {
        for (int b = 0; b < 4; b++) {
            k ^= (v[i] >> (b * 8)) & 0xffu;
            k *= 0x100000001b3ull;
        }
    }
    return k | 1u;   /* never 0: 0 means no shape is remembered */
}

/* Key of everything that decides the composed image apart from the pixels: the
 * buffers, the rectangles and the blend state of every layer, and the CRTC size.
 * Two frames with the same key and the same buffer contents compose the same.
 * FNV-1a over the fields, 64-bit. */
static inline uint64_t drmtap_layers_key(const drmtap_layer_set *s) {
    uint64_t h = 0xcbf29ce484222325ull;
#define DRMTAP_KEY_MIX(v) do { uint64_t _v = (uint64_t)(v); \
        for (int _b = 0; _b < 8; _b++) { h ^= (_v >> (_b * 8)) & 0xffu; \
            h *= 0x100000001b3ull; } } while (0)
    DRMTAP_KEY_MIX(s->crtc_w);
    DRMTAP_KEY_MIX(s->crtc_h);
    DRMTAP_KEY_MIX(s->count);
    for (uint32_t i = 0; i < s->count; i++) {
        const drmtap_layer *l = &s->layers[i];
        DRMTAP_KEY_MIX(l->plane_id);
        DRMTAP_KEY_MIX(l->fb_id);
        DRMTAP_KEY_MIX(l->src_x);
        DRMTAP_KEY_MIX(l->src_y);
        DRMTAP_KEY_MIX(l->src_w);
        DRMTAP_KEY_MIX(l->src_h);
        DRMTAP_KEY_MIX((uint32_t)l->crtc_x);
        DRMTAP_KEY_MIX((uint32_t)l->crtc_y);
        DRMTAP_KEY_MIX(l->crtc_w);
        DRMTAP_KEY_MIX(l->crtc_h);
        DRMTAP_KEY_MIX(l->zpos);
        DRMTAP_KEY_MIX(l->alpha);
        DRMTAP_KEY_MIX(l->blend);
        DRMTAP_KEY_MIX(l->rotation);
    }
#undef DRMTAP_KEY_MIX
    return h;
}

/* 1 when both sets describe the same scanout: same planes, same framebuffers, same
 * rectangles and blend state. Used to tell whether the compositor flipped while
 * the buffers were being exported. */
static inline int drmtap_layers_same(const drmtap_layer_set *a, const drmtap_layer_set *b) {
    if (a->count != b->count || a->crtc_w != b->crtc_w || a->crtc_h != b->crtc_h) {
        return 0;
    }
    for (uint32_t i = 0; i < a->count; i++) {
        const drmtap_layer *x = &a->layers[i], *y = &b->layers[i];
        if (x->plane_id != y->plane_id || x->fb_id != y->fb_id ||
            x->src_x != y->src_x || x->src_y != y->src_y ||
            x->src_w != y->src_w || x->src_h != y->src_h ||
            x->crtc_x != y->crtc_x || x->crtc_y != y->crtc_y ||
            x->crtc_w != y->crtc_w || x->crtc_h != y->crtc_h ||
            x->zpos != y->zpos || x->alpha != y->alpha ||
            x->blend != y->blend || x->rotation != y->rotation ||
            x->color_encoding != y->color_encoding || x->color_range != y->color_range) {
            return 0;
        }
    }
    return 1;
}

/* What drmtap_grab_desc() does about an underlay (the primary plane does not
 * cover the CRTC, so its one buffer is not the screen). For the first
 * DRMTAP_UNDERLAY_EAGAIN_MS of a continuous underlay: -EAGAIN, the consumer keeps
 * its last frame (KWin's underlays while a window opens lasted 1.1-1.4 s here).
 * After that, a consumer that counts stalls (rustdesk closes after ~5 s) would
 * drop the capture, so if some lit layer covers the whole CRTC that layer is
 * returned instead: the size stays the CRTC's and the window under it shows as
 * its transparent hole. With no covering layer, -EAGAIN goes on.
 *
 * Pure, the clock is passed in, so the timing is unit-tested. *since_ms is the
 * start of the current underlay (0 = none) and is kept by the caller per context.
 * `underlay`: this frame is one; `have_cover`: a lit layer covers the CRTC. */
#define DRMTAP_UNDERLAY_EAGAIN_MS 2000
#define DRMTAP_UNDERLAY_NONE   0   /* not an underlay: the normal path */
#define DRMTAP_UNDERLAY_EAGAIN 1   /* return -EAGAIN */
#define DRMTAP_UNDERLAY_COVER  2   /* return the covering layer */
static inline int drmtap_underlay_decide(int64_t *since_ms, int64_t now_ms, int underlay,
                                         int have_cover, int64_t limit_ms) {
    if (!underlay) {
        *since_ms = 0;
        return DRMTAP_UNDERLAY_NONE;
    }
    if (*since_ms == 0) {
        *since_ms = now_ms > 0 ? now_ms : 1;
    }
    if (now_ms - *since_ms < limit_ms) {
        return DRMTAP_UNDERLAY_EAGAIN;
    }
    return have_cover ? DRMTAP_UNDERLAY_COVER : DRMTAP_UNDERLAY_EAGAIN;
}

/* 1 for a rotation value the kernel can hold: exactly one rotate bit, and only
 * the rotate and reflect bits. */
static inline int drmtap_rotation_valid(uint32_t r) {
    uint32_t rot = r & DRMTAP_ROT_MASK;
    if (r & ~(DRMTAP_ROT_MASK | DRMTAP_REFLECT_X | DRMTAP_REFLECT_Y)) {
        return 0;
    }
    return rot == DRMTAP_ROT_0 || rot == DRMTAP_ROT_90 ||
           rot == DRMTAP_ROT_180 || rot == DRMTAP_ROT_270;
}

/* The quad that draws layer `l` into a w x h CRTC, as a 4-vertex triangle strip in
 * the order top-left, top-right, bottom-left, bottom-right OF THE DESTINATION
 * (image coordinates, y down). pos[] is normalized device coordinates where y=-1
 * is the top image row: the EGL readback writes framebuffer row 0 first, so the
 * output keeps image row 0 first without a flip. tex[] is the normalized texture
 * coordinate in the framebuffer that lands on each corner, v = 0 at buffer row 0.
 *
 * Rotation is applied the way drm_rect_rotate() does: reflect first, then rotate
 * counter-clockwise. Only ROTATE_0 has been seen on hardware here (KWin and
 * mutter on i915); the other values follow the kernel definition and are covered
 * by the unit test, not by a capture. */
static inline void drmtap_layer_quad(const drmtap_layer *l, uint32_t w, uint32_t h,
                                     float pos[8], float tex[8]) {
    double x0 = (double)l->crtc_x, y0 = (double)l->crtc_y;
    double x1 = x0 + (double)l->crtc_w, y1 = y0 + (double)l->crtc_h;
    double W = w ? (double)w : 1.0, H = h ? (double)h : 1.0;
    float nx0 = (float)(2.0 * x0 / W - 1.0), nx1 = (float)(2.0 * x1 / W - 1.0);
    float ny0 = (float)(2.0 * y0 / H - 1.0), ny1 = (float)(2.0 * y1 / H - 1.0);
    pos[0] = nx0; pos[1] = ny0;   /* TL */
    pos[2] = nx1; pos[3] = ny0;   /* TR */
    pos[4] = nx0; pos[5] = ny1;   /* BL */
    pos[6] = nx1; pos[7] = ny1;   /* BR */

    double fw = l->fb_width ? (double)l->fb_width * 65536.0 : 1.0;
    double fh = l->fb_height ? (double)l->fb_height * 65536.0 : 1.0;
    float u0 = (float)((double)l->src_x / fw);
    float u1 = (float)(((double)l->src_x + (double)l->src_w) / fw);
    float v0 = (float)((double)l->src_y / fh);
    float v1 = (float)(((double)l->src_y + (double)l->src_h) / fh);
    /* Source corners in the same TL, TR, BL, BR order. */
    float s[8] = { u0, v0, u1, v0, u0, v1, u1, v1 };
    float t[8];
    if (l->rotation & DRMTAP_REFLECT_X) {          /* mirror left/right */
        memcpy(t, s, sizeof(t));
        s[0] = t[2]; s[1] = t[3]; s[2] = t[0]; s[3] = t[1];
        s[4] = t[6]; s[5] = t[7]; s[6] = t[4]; s[7] = t[5];
    }
    if (l->rotation & DRMTAP_REFLECT_Y) {          /* mirror top/bottom */
        memcpy(t, s, sizeof(t));
        s[0] = t[4]; s[1] = t[5]; s[2] = t[6]; s[3] = t[7];
        s[4] = t[0]; s[5] = t[1]; s[6] = t[2]; s[7] = t[3];
    }
    /* map[k] = which source corner lands on destination corner k. */
    static const int rot0[4] = { 0, 1, 2, 3 };
    static const int rot90[4] = { 1, 3, 0, 2 };   /* TL<-TR, TR<-BR, BL<-TL, BR<-BL */
    static const int rot180[4] = { 3, 2, 1, 0 };
    static const int rot270[4] = { 2, 0, 3, 1 };  /* TL<-BL, TR<-TL, BL<-BR, BR<-TR */
    const int *map = rot0;
    switch (l->rotation & DRMTAP_ROT_MASK) {
    case DRMTAP_ROT_90:  map = rot90;  break;
    case DRMTAP_ROT_180: map = rot180; break;
    case DRMTAP_ROT_270: map = rot270; break;
    default:             map = rot0;   break;
    }
    for (int k = 0; k < 4; k++) {
        tex[k * 2] = s[map[k] * 2];
        tex[k * 2 + 1] = s[map[k] * 2 + 1];
    }
}

/* ========================================================================= */
/* Packet: the bytes a split exporter hands to its converter                 */
/* ========================================================================= */
/*
 * Little-endian, fixed field offsets, version 1:
 *
 *   header (36 bytes)
 *     0  u32 magic "DTPK"        4  u16 version (1)     6  u16 header length (36)
 *     8  u32 total length       12  u16 layer count    14  u16 layer record length (128)
 *    16  u16 fd count           18  u16 flags (0)      20  u32 crtc width
 *    24  u32 crtc height        28  u32 hdr eotf       32  u32 hdr max nits
 *   layer record (128 bytes), bottom to top
 *     0 plane_id   4 fb_id   8 fb_width  12 fb_height  16 format  20 num_planes
 *    24 u64 modifier         32 offsets[4]             48 pitches[4]
 *    64 u8 fd_index[4]       68 src_x  72 src_y  76 src_w  80 src_h
 *    84 i32 crtc_x  88 i32 crtc_y  92 crtc_w  96 crtc_h  100 zpos  104 alpha
 *   108 blend  112 rotation  116 color_encoding  120 color_range  124 flags
 *
 * The decoder treats the bytes as hostile: every count, length, index and
 * rectangle is checked before anything is used, and it reads nothing outside
 * [0, len). Version 1 requires the exact header and record lengths; a later
 * version that grows a record must bump the version.
 */
#define DRMTAP_PKT_MAGIC       0x4B505444u  /* "DTPK" read as little-endian u32 */
#define DRMTAP_PKT_VERSION     1u
#define DRMTAP_PKT_HDR_LEN     36u
#define DRMTAP_PKT_LAYER_LEN   128u
#define DRMTAP_PKT_MAX_LEN     (DRMTAP_PKT_HDR_LEN + DRMTAP_MAX_LAYERS * DRMTAP_PKT_LAYER_LEN)
#define DRMTAP_PKT_NO_FD       0xffu

static inline void drmtap_pkt_put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void drmtap_pkt_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void drmtap_pkt_put64(uint8_t *p, uint64_t v) {
    drmtap_pkt_put32(p, (uint32_t)v);
    drmtap_pkt_put32(p + 4, (uint32_t)(v >> 32));
}
static inline uint32_t drmtap_pkt_get16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static inline uint32_t drmtap_pkt_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t drmtap_pkt_get64(const uint8_t *p) {
    return (uint64_t)drmtap_pkt_get32(p) | ((uint64_t)drmtap_pkt_get32(p + 4) << 32);
}

/* Write `s` (layers and fd count; the fds themselves travel out of band) into
 * buf. Returns the byte length, or -EINVAL when the set itself is out of range
 * (so a producer bug fails here, not at the far end), -ENOSPC when cap is short. */
static inline int drmtap_layer_valid(const drmtap_layer *l, uint32_t nfds);
static inline int drmtap_packet_encode(const drmtap_layer_set *s, uint8_t *buf, size_t cap) {
    if (!s || !buf || s->count == 0 || s->count > DRMTAP_MAX_LAYERS ||
        s->nfds == 0 || s->nfds > DRMTAP_LAYERS_MAX_FDS) {
        return -EINVAL;
    }
    for (uint32_t i = 0; i < s->count; i++) {
        if (!drmtap_layer_valid(&s->layers[i], s->nfds)) {
            return -EINVAL;
        }
    }
    size_t len = DRMTAP_PKT_HDR_LEN + (size_t)s->count * DRMTAP_PKT_LAYER_LEN;
    if (cap < len) {
        return -ENOSPC;
    }
    memset(buf, 0, len);
    drmtap_pkt_put32(buf + 0, DRMTAP_PKT_MAGIC);
    drmtap_pkt_put16(buf + 4, DRMTAP_PKT_VERSION);
    drmtap_pkt_put16(buf + 6, DRMTAP_PKT_HDR_LEN);
    drmtap_pkt_put32(buf + 8, (uint32_t)len);
    drmtap_pkt_put16(buf + 12, s->count);
    drmtap_pkt_put16(buf + 14, DRMTAP_PKT_LAYER_LEN);
    drmtap_pkt_put16(buf + 16, s->nfds);
    drmtap_pkt_put16(buf + 18, 0);
    drmtap_pkt_put32(buf + 20, s->crtc_w);
    drmtap_pkt_put32(buf + 24, s->crtc_h);
    drmtap_pkt_put32(buf + 28, s->hdr_eotf);
    drmtap_pkt_put32(buf + 32, s->hdr_max_nits);
    for (uint32_t i = 0; i < s->count; i++) {
        const drmtap_layer *l = &s->layers[i];
        uint8_t *r = buf + DRMTAP_PKT_HDR_LEN + (size_t)i * DRMTAP_PKT_LAYER_LEN;
        drmtap_pkt_put32(r + 0, l->plane_id);
        drmtap_pkt_put32(r + 4, l->fb_id);
        drmtap_pkt_put32(r + 8, l->fb_width);
        drmtap_pkt_put32(r + 12, l->fb_height);
        drmtap_pkt_put32(r + 16, l->format);
        drmtap_pkt_put32(r + 20, l->num_planes);
        drmtap_pkt_put64(r + 24, l->modifier);
        for (int p = 0; p < 4; p++) {
            drmtap_pkt_put32(r + 32 + p * 4, l->offsets[p]);
            drmtap_pkt_put32(r + 48 + p * 4, l->pitches[p]);
            r[64 + p] = l->fd_index[p];
        }
        drmtap_pkt_put32(r + 68, l->src_x);
        drmtap_pkt_put32(r + 72, l->src_y);
        drmtap_pkt_put32(r + 76, l->src_w);
        drmtap_pkt_put32(r + 80, l->src_h);
        drmtap_pkt_put32(r + 84, (uint32_t)l->crtc_x);
        drmtap_pkt_put32(r + 88, (uint32_t)l->crtc_y);
        drmtap_pkt_put32(r + 92, l->crtc_w);
        drmtap_pkt_put32(r + 96, l->crtc_h);
        drmtap_pkt_put32(r + 100, l->zpos);
        drmtap_pkt_put32(r + 104, l->alpha);
        drmtap_pkt_put32(r + 108, l->blend);
        drmtap_pkt_put32(r + 112, l->rotation);
        drmtap_pkt_put32(r + 116, l->color_encoding);
        drmtap_pkt_put32(r + 120, l->color_range);
        drmtap_pkt_put32(r + 124, l->flags);
    }
    return (int)len;
}

/* Validate one decoded layer against the fd count it may index. Geometry only;
 * that each fd is a dma-buf large enough is checked by the caller, which owns
 * the fds. */
static inline int drmtap_layer_valid(const drmtap_layer *l, uint32_t nfds) {
    if (l->fb_width == 0 || l->fb_height == 0 ||
        l->fb_width > DRMTAP_LAYER_MAX_DIM || l->fb_height > DRMTAP_LAYER_MAX_DIM) {
        return 0;
    }
    if (l->num_planes < 1 || l->num_planes > 4 || l->pitches[0] == 0) {
        return 0;
    }
    for (uint32_t p = 0; p < 4; p++) {
        if (p < l->num_planes) {
            if (l->fd_index[p] >= nfds) {
                return 0;
            }
            /* EGL takes offsets and pitches as EGLint; a used plane has a row. */
            if (l->offsets[p] > 0x7fffffffu || l->pitches[p] > 0x7fffffffu ||
                l->pitches[p] == 0) {
                return 0;
            }
        } else if (l->fd_index[p] != DRMTAP_PKT_NO_FD || l->offsets[p] || l->pitches[p]) {
            return 0;
        }
    }
    /* The source rectangle must lie inside the framebuffer and not be empty. */
    if (l->src_w == 0 || l->src_h == 0 ||
        (uint64_t)l->src_x + l->src_w > ((uint64_t)l->fb_width << 16) ||
        (uint64_t)l->src_y + l->src_h > ((uint64_t)l->fb_height << 16)) {
        return 0;
    }
    if (l->crtc_w == 0 || l->crtc_h == 0 ||
        l->crtc_w > DRMTAP_LAYER_MAX_DIM || l->crtc_h > DRMTAP_LAYER_MAX_DIM ||
        l->crtc_x < -(int32_t)DRMTAP_LAYER_MAX_DIM || l->crtc_x > (int32_t)DRMTAP_LAYER_MAX_DIM ||
        l->crtc_y < -(int32_t)DRMTAP_LAYER_MAX_DIM || l->crtc_y > (int32_t)DRMTAP_LAYER_MAX_DIM) {
        return 0;
    }
    if (l->alpha > 0xffffu || l->blend > DRMTAP_BLEND_NONE ||
        !drmtap_rotation_valid(l->rotation) ||
        l->color_encoding > DRMTAP_YCBCR_BT2020 || l->color_range > DRMTAP_RANGE_FULL ||
        (l->flags & ~DRMTAP_LAYER_FLAG_PRIMARY)) {
        return 0;
    }
    return 1;
}

/* Parse `len` bytes into *s. `nfds` is how many fds actually arrived with the
 * bytes; the packet must declare exactly that many and every plane must index
 * one of them. On success s->fds[] is left at -1 for the caller to fill.
 * Returns 0, -EINVAL (malformed), -EPROTO (not a libdrmtap packet) or
 * -EPROTONOSUPPORT (a packet version this build does not read). */
static inline int drmtap_packet_decode(const uint8_t *buf, size_t len, size_t nfds,
                                       drmtap_layer_set *s) {
    if (!s) {
        return -EINVAL;
    }
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < DRMTAP_LAYERS_MAX_FDS; i++) {
        s->fds[i] = -1;
    }
    if (!buf || len < DRMTAP_PKT_HDR_LEN) {
        return -EINVAL;
    }
    if (drmtap_pkt_get32(buf) != DRMTAP_PKT_MAGIC) {
        return -EPROTO;
    }
    if (drmtap_pkt_get16(buf + 4) != DRMTAP_PKT_VERSION) {
        return -EPROTONOSUPPORT;
    }
    uint32_t hlen = drmtap_pkt_get16(buf + 6);
    uint32_t total = drmtap_pkt_get32(buf + 8);
    uint32_t count = drmtap_pkt_get16(buf + 12);
    uint32_t rlen = drmtap_pkt_get16(buf + 14);
    uint32_t pfds = drmtap_pkt_get16(buf + 16);
    uint32_t flags = drmtap_pkt_get16(buf + 18);
    if (hlen != DRMTAP_PKT_HDR_LEN || rlen != DRMTAP_PKT_LAYER_LEN || flags != 0 ||
        count == 0 || count > DRMTAP_MAX_LAYERS ||
        (size_t)total != len ||
        (size_t)total != DRMTAP_PKT_HDR_LEN + (size_t)count * DRMTAP_PKT_LAYER_LEN ||
        pfds == 0 || pfds > DRMTAP_LAYERS_MAX_FDS || (size_t)pfds != nfds) {
        return -EINVAL;
    }
    s->count = count;
    s->lit = count;
    s->nfds = pfds;
    s->crtc_w = drmtap_pkt_get32(buf + 20);
    s->crtc_h = drmtap_pkt_get32(buf + 24);
    s->hdr_eotf = drmtap_pkt_get32(buf + 28);
    s->hdr_max_nits = drmtap_pkt_get32(buf + 32);
    if (s->crtc_w == 0 || s->crtc_h == 0 ||
        s->crtc_w > DRMTAP_LAYER_MAX_DIM || s->crtc_h > DRMTAP_LAYER_MAX_DIM) {
        return -EINVAL;
    }
    for (uint32_t i = 0; i < count; i++) {
        drmtap_layer *l = &s->layers[i];
        const uint8_t *r = buf + DRMTAP_PKT_HDR_LEN + (size_t)i * DRMTAP_PKT_LAYER_LEN;
        l->plane_id = drmtap_pkt_get32(r + 0);
        l->fb_id = drmtap_pkt_get32(r + 4);
        l->fb_width = drmtap_pkt_get32(r + 8);
        l->fb_height = drmtap_pkt_get32(r + 12);
        l->format = drmtap_pkt_get32(r + 16);
        l->num_planes = drmtap_pkt_get32(r + 20);
        l->modifier = drmtap_pkt_get64(r + 24);
        for (int p = 0; p < 4; p++) {
            l->offsets[p] = drmtap_pkt_get32(r + 32 + p * 4);
            l->pitches[p] = drmtap_pkt_get32(r + 48 + p * 4);
            l->fd_index[p] = r[64 + p];
        }
        l->src_x = drmtap_pkt_get32(r + 68);
        l->src_y = drmtap_pkt_get32(r + 72);
        l->src_w = drmtap_pkt_get32(r + 76);
        l->src_h = drmtap_pkt_get32(r + 80);
        l->crtc_x = (int32_t)drmtap_pkt_get32(r + 84);
        l->crtc_y = (int32_t)drmtap_pkt_get32(r + 88);
        l->crtc_w = drmtap_pkt_get32(r + 92);
        l->crtc_h = drmtap_pkt_get32(r + 96);
        l->zpos = drmtap_pkt_get32(r + 100);
        l->alpha = drmtap_pkt_get32(r + 104);
        l->blend = drmtap_pkt_get32(r + 108);
        l->rotation = drmtap_pkt_get32(r + 112);
        l->color_encoding = drmtap_pkt_get32(r + 116);
        l->color_range = drmtap_pkt_get32(r + 120);
        l->flags = drmtap_pkt_get32(r + 124);
        if (!drmtap_layer_valid(l, pfds)) {
            return -EINVAL;
        }
        /* Bottom to top is part of the format, not something to repair here: a
         * reordered list would composite a different image than the panel shows. */
        if (i > 0 && (l->zpos < s->layers[i - 1].zpos ||
                      (l->zpos == s->layers[i - 1].zpos &&
                       l->plane_id <= s->layers[i - 1].plane_id))) {
            return -EINVAL;
        }
    }
    return 0;
}

#endif /* DRMTAP_LAYERS_H */
