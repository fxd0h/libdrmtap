/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file pixel_convert.c
 * @brief Pixel format conversion, GPU tiling deswizzle, and frame differencing
 *
 * Tiling formats handled:
 *   - Intel X-TILED: 512 bytes wide × 8 rows (128 pixels × 8 rows at 32bpp)
 *   - Intel Y-TILED: 128 bytes wide × 32 rows (32 pixels × 32 rows at 32bpp)
 *   - Nvidia X-TILED: 16 pixels × 128 rows per tile
 *   - Linear: no conversion needed
 *
 * Pixel format conversions:
 *   - AR30/XR30 (10-bit per channel) → XRGB8888
 *   - ABGR8888 → ARGB8888 (channel swap)
 */

#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>

#include "drmtap.h"

/* ========================================================================= */
/* Intel tiling deswizzle                                                    */
/* ========================================================================= */

/**
 * Intel X-TILED layout:
 *   Each tile is 512 bytes wide × 8 rows.
 *   At 32bpp (4 bytes/pixel), each tile is 128 pixels × 8 rows.
 *   Tiles are laid out left-to-right, top-to-bottom in memory.
 */
#define INTEL_X_TILE_WIDTH  512   /* bytes */
#define INTEL_X_TILE_HEIGHT 8     /* rows */

static int deswizzle_intel_x_tiled(const void *src, void *dst,
                                   uint32_t width, uint32_t height,
                                   uint32_t src_stride, uint32_t dst_stride,
                                   size_t src_size) {
    uint32_t bpp = 4;  /* bytes per pixel (XRGB8888) */
    uint32_t tile_w_bytes = INTEL_X_TILE_WIDTH;
    uint32_t tile_h = INTEL_X_TILE_HEIGHT;
    uint32_t tiles_x = (src_stride + tile_w_bytes - 1) / tile_w_bytes;

    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;

    for (uint32_t y = 0; y < height; y++) {
        uint32_t tile_row = y / tile_h;
        uint32_t tile_y = y % tile_h;

        for (uint32_t x = 0; x < width; x++) {
            uint32_t x_bytes = x * bpp;
            uint32_t tile_col = x_bytes / tile_w_bytes;
            uint32_t tile_x_bytes = x_bytes % tile_w_bytes;

            /* Offset in tiled memory */
            uint32_t tile_idx = tile_row * tiles_x + tile_col;
            uint32_t tile_size = tile_w_bytes * tile_h;
            uint32_t src_off = tile_idx * tile_size +
                               tile_y * tile_w_bytes + tile_x_bytes;

            /* The tiled footprint assumes ceil(height/tile_h) FULL tile rows,
             * but the source is only src_size bytes (stride*height). Bound every
             * read so a scanout whose height is not a tile multiple can never
             * read past the source; zero-fill the unbacked bottom pixels. */
            if ((size_t)src_off + bpp <= src_size) {
                memcpy(d + y * dst_stride + x * bpp, s + src_off, bpp);
            } else {
                memset(d + y * dst_stride + x * bpp, 0, bpp);
            }
        }
    }

    return 0;
}

/**
 * Intel Y-TILED layout:
 *   Each tile is 128 bytes wide × 32 rows.
 *   At 32bpp, each tile is 32 pixels × 32 rows.
 *   Within each tile, data is organized in 16-byte (OWORD) columns.
 */
#define INTEL_Y_TILE_WIDTH  128   /* bytes */
#define INTEL_Y_TILE_HEIGHT 32    /* rows */
#define INTEL_Y_OWORD       16    /* bytes per column segment */

static int deswizzle_intel_y_tiled(const void *src, void *dst,
                                   uint32_t width, uint32_t height,
                                   uint32_t src_stride, uint32_t dst_stride,
                                   size_t src_size) {
    uint32_t bpp = 4;
    uint32_t tile_w_bytes = INTEL_Y_TILE_WIDTH;
    uint32_t tile_h = INTEL_Y_TILE_HEIGHT;
    uint32_t tiles_x = (src_stride + tile_w_bytes - 1) / tile_w_bytes;
    uint32_t oword = INTEL_Y_OWORD;
    uint32_t columns_per_tile = tile_w_bytes / oword;  /* 8 columns */

    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;

    for (uint32_t y = 0; y < height; y++) {
        uint32_t tile_row = y / tile_h;
        uint32_t tile_y = y % tile_h;

        for (uint32_t x = 0; x < width; x++) {
            uint32_t x_bytes = x * bpp;
            uint32_t tile_col = x_bytes / tile_w_bytes;
            uint32_t in_tile_x = x_bytes % tile_w_bytes;

            /* Y-tiled: column-major within tile */
            uint32_t column = in_tile_x / oword;
            uint32_t column_offset = in_tile_x % oword;

            uint32_t tile_idx = tile_row * tiles_x + tile_col;
            uint32_t tile_size = tile_w_bytes * tile_h;
            uint32_t src_off = tile_idx * tile_size +
                               column * (oword * tile_h) +
                               tile_y * oword +
                               column_offset;

            /* Bound the read: the tiled footprint rounds height up to tile_h,
             * but the source is only src_size bytes. Zero-fill anything past it
             * rather than reading out of bounds. */
            if ((size_t)src_off + bpp <= src_size) {
                memcpy(d + y * dst_stride + x * bpp, s + src_off, bpp);
            } else {
                memset(d + y * dst_stride + x * bpp, 0, bpp);
            }
        }
    }

    (void)columns_per_tile;
    return 0;
}

/* ========================================================================= */
/* Nvidia tiling deswizzle                                                   */
/* ========================================================================= */

/**
 * Nvidia X-TILED (blocklinear) layout:
 *   Each tile is 16 pixels wide × 128 rows.
 *   At 32bpp, each tile is 64 bytes wide × 128 rows = 8192 bytes.
 *   Reference: kmsvnc convert_nvidia_x_tiled_kmsbuf()
 */
#define NV_TILE_WIDTH  16     /* pixels */
#define NV_TILE_HEIGHT 128    /* rows */

static int deswizzle_nvidia_x_tiled(const void *src, void *dst,
                                    uint32_t width, uint32_t height,
                                    uint32_t src_stride, uint32_t dst_stride,
                                    size_t src_size) {
    uint32_t bpp = 4;
    uint32_t tile_w = NV_TILE_WIDTH;
    uint32_t tile_h = NV_TILE_HEIGHT;
    uint32_t tile_w_bytes = tile_w * bpp;
    uint32_t tiles_x = (width + tile_w - 1) / tile_w;

    (void)src_stride;

    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;

    for (uint32_t y = 0; y < height; y++) {
        uint32_t tile_row = y / tile_h;
        uint32_t tile_y = y % tile_h;

        for (uint32_t tx = 0; tx < tiles_x; tx++) {
            uint32_t tile_idx = tile_row * tiles_x + tx;
            uint32_t tile_size = tile_w_bytes * tile_h;
            uint32_t src_off = tile_idx * tile_size + tile_y * tile_w_bytes;

            uint32_t dst_x = tx * tile_w;
            uint32_t copy_w = (dst_x + tile_w > width)
                              ? width - dst_x : tile_w;

            /* Bound the row copy by the real source size (the block-linear
             * footprint rounds height up to tile_h): copy only what is backed,
             * zero-fill the rest, never read past src_size. */
            uint8_t *drow = d + y * dst_stride + dst_x * bpp;
            size_t want = (size_t)copy_w * bpp;
            if ((size_t)src_off >= src_size) {
                memset(drow, 0, want);
            } else {
                size_t avail = src_size - (size_t)src_off;
                if (avail >= want) {
                    memcpy(drow, s + src_off, want);
                } else {
                    memcpy(drow, s + src_off, avail);
                    memset(drow + avail, 0, want - avail);
                }
            }
        }
    }

    return 0;
}

/* ========================================================================= */
/* Pixel format conversion                                                   */
/* ========================================================================= */

/**
 * Convert AR30/XR30 (10-bit per channel, 2-bit alpha) to XRGB8888.
 *
 * AR30/XR30 layout (32 bits):
 *   [31:30] = alpha (2 bits)
 *   [29:20] = red   (10 bits)
 *   [19:10] = green (10 bits)
 *   [9:0]   = blue  (10 bits)
 *
 * We shift each 10-bit channel right by 2 to get 8-bit values.
 */
static void convert_ar30_to_xrgb8888(const void *src, void *dst,
                                     uint32_t width, uint32_t height,
                                     uint32_t src_stride,
                                     uint32_t dst_stride, int bgr) {
    for (uint32_t y = 0; y < height; y++) {
        const uint32_t *s = (const uint32_t *)
            ((const uint8_t *)src + y * src_stride);
        uint32_t *d = (uint32_t *)
            ((uint8_t *)dst + y * dst_stride);

        for (uint32_t x = 0; x < width; x++) {
            uint32_t pixel = s[x];
            /* Top 8 bits of each 10-bit channel. RGB: R[29:20] G[19:10] B[9:0];
             * BGR (XB30/AB30) swaps the two outer channels. */
            uint8_t c_hi = (uint8_t)((pixel >> 22) & 0xFF);  /* [29:20] >> 2 */
            uint8_t g = (uint8_t)((pixel >> 12) & 0xFF);     /* [19:10] >> 2 */
            uint8_t c_lo = (uint8_t)((pixel >> 2) & 0xFF);   /* [9:0] >> 2 */
            uint8_t r = bgr ? c_lo : c_hi;
            uint8_t b = bgr ? c_hi : c_lo;
            d[x] = (0xFFu << 24) | ((uint32_t)r << 16) |
                   ((uint32_t)g << 8) | b;
        }
    }
}

/**
 * Convert ABGR8888 to ARGB8888 (swap R and B channels).
 */
static void convert_abgr_to_argb(const void *src, void *dst,
                                 uint32_t width, uint32_t height,
                                 uint32_t src_stride, uint32_t dst_stride) {
    for (uint32_t y = 0; y < height; y++) {
        const uint32_t *s = (const uint32_t *)
            ((const uint8_t *)src + y * src_stride);
        uint32_t *d = (uint32_t *)
            ((uint8_t *)dst + y * dst_stride);

        for (uint32_t x = 0; x < width; x++) {
            uint32_t pixel = s[x];
            /* ABGR: [A][B][G][R] → ARGB: [A][R][G][B] */
            uint8_t a = (uint8_t)(pixel >> 24);
            uint8_t b_ch = (uint8_t)(pixel >> 16);
            uint8_t g = (uint8_t)(pixel >> 8);
            uint8_t r = (uint8_t)(pixel);
            d[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                   ((uint32_t)g << 8) | b_ch;
        }
    }
}

/* ========================================================================= */
/* Public API — Deswizzle                                                    */
/* ========================================================================= */

int drmtap_deswizzle(const void *src, void *dst,
                     uint32_t width, uint32_t height,
                     uint32_t src_stride, uint32_t dst_stride,
                     uint64_t modifier, size_t src_size) {
    if (!src || !dst || width == 0 || height == 0) {
        return -EINVAL;
    }

    /*
     * DRM format modifiers (from drm_fourcc.h):
     *   DRM_FORMAT_MOD_LINEAR        = 0
     *   I915_FORMAT_MOD_X_TILED      = 0x0100000000000001
     *   I915_FORMAT_MOD_Y_TILED      = 0x0100000000000002
     *   (Nvidia uses vendor-specific modifiers)
     *
     * We detect the vendor from the modifier's vendor bits.
     */

    /* Linear — just memcpy row by row (bounded by src_size; a full row always
     * fits since width*4 <= src_stride, but guard the last rows defensively). */
    if (modifier == 0 /* DRM_FORMAT_MOD_LINEAR */) {
        for (uint32_t y = 0; y < height; y++) {
            size_t row_off = (size_t)y * src_stride;
            if (row_off + (size_t)width * 4 > src_size) {
                /* Zero-fill unbacked rows instead of leaving the dst buffer
                 * (malloc'd, not zeroed) with stale/uninitialized bytes. */
                memset((uint8_t *)dst + y * dst_stride, 0, (size_t)width * 4);
                continue;
            }
            memcpy((uint8_t *)dst + y * dst_stride,
                   (const uint8_t *)src + row_off,
                   width * 4);
        }
        return 0;
    }

    /* Intel modifiers: vendor = 0x01 */
    uint8_t vendor = (uint8_t)(modifier >> 56);
    uint8_t mod_type = (uint8_t)(modifier & 0xFF);

    if (vendor == 0x01) {
        if (mod_type == 0x01) {
            /* I915_FORMAT_MOD_X_TILED */
            return deswizzle_intel_x_tiled(src, dst, width, height,
                                           src_stride, dst_stride, src_size);
        } else if (mod_type == 0x02 || mod_type == 0x03) {
            /* I915_FORMAT_MOD_Y_TILED (0x02)
             * I915_FORMAT_MOD_Yf_TILED (0x03)
             *
             * Pure Y-tiled without compression — CPU deswizzle works. */
            return deswizzle_intel_y_tiled(src, dst, width, height,
                                           src_stride, dst_stride, src_size);
        } else if (mod_type == 0x05 || mod_type == 0x06 ||
                   mod_type == 0x07 || mod_type == 0x08) {
            /* I915_FORMAT_MOD_Y_TILED_CCS (0x05)
             * I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS (0x06)
             * I915_FORMAT_MOD_Y_TILED_GEN12_MC_CCS (0x07)
             * I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS_CC (0x08)
             *
             * CCS (Color Compression Surface) variants. The pixel data
             * is GPU-compressed and CANNOT be CPU-deswizzled.
             * A dumb_mmap of CCS-compressed buffers returns garbage —
             * only EGL import (via DMA-BUF fd) can decompress correctly.
             *
             * In the helper path (dma_buf_fd == -1), pixels arrive via
             * socket from a dumb_mmap, so they are still CCS-compressed.
             * We return -ENOTSUP to signal that GPU deswizzle (EGL) or
             * DMA-BUF fd passing (SCM_RIGHTS) is required. */
            return -ENOTSUP;
        }
    }

    /* Nvidia modifier: vendor = 0x10 (NVIDIA) */
    if (vendor == 0x10) {
        return deswizzle_nvidia_x_tiled(src, dst, width, height,
                                         src_stride, dst_stride, src_size);
    }

    /* Unknown modifier — try linear copy as fallback (bounded by src_size). */
    for (uint32_t y = 0; y < height; y++) {
        size_t row_off = (size_t)y * src_stride;
        if (row_off + (size_t)width * 4 > src_size) {
            /* Zero-fill unbacked rows rather than leaking uninitialized dst. */
            memset((uint8_t *)dst + y * dst_stride, 0, (size_t)width * 4);
            continue;
        }
        memcpy((uint8_t *)dst + y * dst_stride,
               (const uint8_t *)src + row_off,
               width * 4);
    }
    return 0;
}

/* ========================================================================= */
/* Public API — Format Conversion                                            */
/* ========================================================================= */

/* ========================================================================= */
/* HDR10 (PQ / BT.2020) -> SDR (BT.709 / sRGB) tone mapping                  */
/* ========================================================================= */

/* BT.2408 reference graphics/diffuse white. HDR diffuse white sits here; this
 * is the luminance we map to SDR "1.0" so SDR-range content keeps its normal
 * brightness and only true highlights above it get rolled off. */
#define DRMTAP_SDR_WHITE_NITS 203.0

#define DRMTAP_PQ_LUT_N   1024    /* one entry per 10-bit code value */
#define DRMTAP_PQ_LUT16_N 65536   /* one entry per 16-bit code value */
#define DRMTAP_SRGB_LUT_N 4096    /* linear [0,1] -> 8-bit sRGB */

static float   g_pq_lut[DRMTAP_PQ_LUT_N];      /* 10-bit code -> luminance (nits) */
static float   g_pq_lut16[DRMTAP_PQ_LUT16_N];  /* 16-bit code -> luminance (nits) */
static uint8_t g_srgb_lut[DRMTAP_SRGB_LUT_N];  /* linear [0,1] -> 8-bit sRGB */
static pthread_once_t g_hdr_once = PTHREAD_ONCE_INIT;

/* SMPTE ST 2084 (PQ) EOTF. Input: normalized code value [0,1].
 * Output: absolute display luminance in nits (cd/m^2), 0..10000. */
static double pq_eotf_nits(double e) {
    const double m1 = 2610.0 / 16384.0;
    const double m2 = 2523.0 / 4096.0 * 128.0;
    const double c1 = 3424.0 / 4096.0;
    const double c2 = 2413.0 / 4096.0 * 32.0;
    const double c3 = 2392.0 / 4096.0 * 32.0;
    if (e <= 0.0) return 0.0;
    double ep = pow(e, 1.0 / m2);
    double num = ep - c1;
    if (num < 0.0) num = 0.0;
    double den = c2 - c3 * ep;
    if (den <= 0.0) return 10000.0;
    return 10000.0 * pow(num / den, 1.0 / m1);
}

/* sRGB OETF: linear [0,1] -> non-linear sRGB [0,1]. */
static double srgb_oetf(double c) {
    if (c <= 0.0) return 0.0;
    if (c >= 1.0) return 1.0;
    if (c <= 0.0031308) return 12.92 * c;
    return 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

static void hdr_lut_init(void) {
    for (int i = 0; i < DRMTAP_PQ_LUT_N; i++) {
        g_pq_lut[i] = (float)pq_eotf_nits((double)i / (DRMTAP_PQ_LUT_N - 1));
    }
    for (int i = 0; i < DRMTAP_PQ_LUT16_N; i++) {
        g_pq_lut16[i] = (float)pq_eotf_nits((double)i / (DRMTAP_PQ_LUT16_N - 1));
    }
    for (int i = 0; i < DRMTAP_SRGB_LUT_N; i++) {
        double s = srgb_oetf((double)i / (DRMTAP_SRGB_LUT_N - 1));
        int v = (int)(s * 255.0 + 0.5);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        g_srgb_lut[i] = (uint8_t)v;
    }
}

/* Highlight-preserving tone curve. Input x is linear light normalized so that
 * 1.0 == SDR diffuse white: x <= knee passes through (SDR content keeps its
 * brightness), and x in (knee, peak_n] rolls off smoothly to 1.0 (HDR
 * highlights, which an SDR display cannot show brighter than white). peak_n is
 * the content peak luminance in the same SDR-white units, so a brighter
 * mastering peak spreads the highlights over more of the top range instead of
 * clipping them all to white. */
static double tonemap_softknee(double x, double peak_n) {
    const double knee = 0.90;
    if (x <= knee) return x;
    if (peak_n < knee + 0.5) peak_n = knee + 0.5;  /* sane minimum roll-off width */
    if (x >= peak_n) return 1.0;
    double t = (x - knee) / (peak_n - knee);        /* [0,1] across the highlights */
    return knee + (1.0 - knee) * (t * (2.0 - t));   /* quadratic ease-out to 1.0 */
}

/* sRGB-encode a linear [0,inf) channel via the precomputed LUT. */
static uint8_t to_srgb8(double linear) {
    if (linear <= 0.0) return g_srgb_lut[0];
    if (linear >= 1.0) return g_srgb_lut[DRMTAP_SRGB_LUT_N - 1];
    int idx = (int)(linear * (DRMTAP_SRGB_LUT_N - 1) + 0.5);
    return g_srgb_lut[idx];
}

/* Map three BT.2020 linear-light (nits) channels through the gamut matrix, the
 * tone curve and the sRGB OETF to SDR 8-bit. */
static void tonemap_rgb_linear(double r_lin, double g_lin, double b_lin,
                               double peak_n,
                               uint8_t *r8, uint8_t *g8, uint8_t *b8) {
    /* BT.2020 -> BT.709 in linear light. */
    double r =  1.660491 * r_lin - 0.587641 * g_lin - 0.072850 * b_lin;
    double g = -0.124550 * r_lin + 1.132900 * g_lin - 0.008349 * b_lin;
    double b = -0.018151 * r_lin - 0.100579 * g_lin + 1.118730 * b_lin;
    if (r < 0.0) r = 0.0;
    if (g < 0.0) g = 0.0;
    if (b < 0.0) b = 0.0;

    /* Normalize to SDR white, roll off highlights, sRGB-encode. */
    *r8 = to_srgb8(tonemap_softknee(r / DRMTAP_SDR_WHITE_NITS, peak_n));
    *g8 = to_srgb8(tonemap_softknee(g / DRMTAP_SDR_WHITE_NITS, peak_n));
    *b8 = to_srgb8(tonemap_softknee(b / DRMTAP_SDR_WHITE_NITS, peak_n));
}

/* content peak (nits) -> tone-curve peak in SDR-white units (default 1000). */
static double hdr_peak_units(uint32_t max_nits) {
    double peak = (max_nits > 0) ? (double)max_nits : 1000.0;
    return peak / DRMTAP_SDR_WHITE_NITS;
}

int drmtap_tonemap_hdr10(const void *src, void *dst,
                         uint32_t width, uint32_t height,
                         uint32_t src_stride, uint32_t dst_stride,
                         uint32_t src_format, uint32_t max_nits) {
    if (!src || !dst || width == 0 || height == 0) {
        return -EINVAL;
    }
    /* XR30/AR30 = X/ARGB2101010 (RGB order), XB30/AB30 = X/ABGR2101010 (BGR). */
    int is_rgb = (src_format == 0x30335258u || src_format == 0x30335241u);
    int is_bgr = (src_format == 0x30334258u || src_format == 0x30334241u);
    if (!is_rgb && !is_bgr) {
        return -ENOTSUP;
    }
    /* Both buffers are 4 bytes/pixel; a stride that cannot hold a full row
     * would make the per-row indexing below over-read or over-write. */
    size_t row_bytes = (size_t)width * 4u;
    if (row_bytes > UINT32_MAX || src_stride < row_bytes ||
        dst_stride < row_bytes) {
        return -EINVAL;
    }

    pthread_once(&g_hdr_once, hdr_lut_init);
    double peak_n = hdr_peak_units(max_nits);

    for (uint32_t y = 0; y < height; y++) {
        const uint32_t *s = (const uint32_t *)
            ((const uint8_t *)src + (size_t)y * src_stride);
        uint32_t *d = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_stride);
        for (uint32_t x = 0; x < width; x++) {
            uint32_t pixel = s[x];
            /* 10-bit channels. RGB: R[29:20] G[19:10] B[9:0]; BGR swaps the two
             * outer channels (B[29:20] R[9:0]). */
            uint32_t c_hi = (pixel >> 20) & 0x3FF;
            uint32_t c_lo = (pixel)       & 0x3FF;
            uint8_t r, g, b;
            tonemap_rgb_linear(g_pq_lut[is_bgr ? c_lo : c_hi],
                               g_pq_lut[(pixel >> 10) & 0x3FF],
                               g_pq_lut[is_bgr ? c_hi : c_lo],
                               peak_n, &r, &g, &b);
            d[x] = (0xFFu << 24) | ((uint32_t)r << 16) |
                   ((uint32_t)g << 8) | b;
        }
    }
    return 0;
}

int drmtap_convert_rgb16(const void *src, void *dst,
                         uint32_t width, uint32_t height,
                         uint32_t src_stride, uint32_t dst_stride,
                         int bgr, uint32_t eotf, uint32_t max_nits) {
    if (!src || !dst || width == 0 || height == 0) {
        return -EINVAL;
    }
    /* Source is 4x16-bit (8 B/px), destination is XRGB8888 (4 B/px). */
    size_t src_row = (size_t)width * 8u;
    size_t dst_row = (size_t)width * 4u;
    if (src_row > UINT32_MAX || dst_row > UINT32_MAX ||
        src_stride < src_row || dst_stride < dst_row) {
        return -EINVAL;
    }

    int hdr = (eotf == 2 /* DRMTAP_EOTF_PQ */);
    if (hdr) {
        pthread_once(&g_hdr_once, hdr_lut_init);
    }
    double peak_n = hdr_peak_units(max_nits);
    /* Memory order of the 16-bit quad is little-endian: XR48 (RGB) lays out
     * B,G,R,X; XB48 (BGR) lays out R,G,B,X. */
    int ri = bgr ? 0 : 2;
    int bi = bgr ? 2 : 0;

    for (uint32_t y = 0; y < height; y++) {
        const uint16_t *s = (const uint16_t *)
            ((const uint8_t *)src + (size_t)y * src_stride);
        uint32_t *d = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_stride);
        for (uint32_t x = 0; x < width; x++) {
            const uint16_t *px = s + (size_t)x * 4;
            uint16_t cr = px[ri], cg = px[1], cb = px[bi];
            uint8_t r8, g8, b8;
            if (hdr) {
                /* 16-bit PQ -> BT.2020 linear nits -> tone-map -> SDR. */
                tonemap_rgb_linear(g_pq_lut16[cr],
                                   g_pq_lut16[cg],
                                   g_pq_lut16[cb],
                                   peak_n, &r8, &g8, &b8);
            } else {
                /* Plain SDR 16-bit -> 8-bit (keep the high byte). */
                r8 = (uint8_t)(cr >> 8);
                g8 = (uint8_t)(cg >> 8);
                b8 = (uint8_t)(cb >> 8);
            }
            d[x] = (0xFFu << 24) | ((uint32_t)r8 << 16) |
                   ((uint32_t)g8 << 8) | b8;
        }
    }
    return 0;
}

/* IEEE 754 binary16 -> binary32. */
static float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;                       /* +/-0 */
        } else {                               /* subnormal */
            exp = 127u - 15u + 1u;
            while ((mant & 0x400u) == 0) { mant <<= 1; exp--; }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1Fu) {                 /* Inf / NaN */
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15u + 127u) << 23) | (mant << 13);
    }
    float out;
    memcpy(&out, &bits, sizeof(out));
    return out;
}

/* Linear light [0,1] -> 8-bit sRGB via the shared OETF LUT (caller runs the
 * pthread_once). Clamps out-of-range (HDR highlights, NaN) into [0,255]. */
static uint8_t srgb8_from_linear(float f) {
    if (!(f > 0.0f)) {           /* <= 0 or NaN */
        return 0;
    }
    if (f >= 1.0f) {
        return 255;
    }
    int i = (int)(f * (float)(DRMTAP_SRGB_LUT_N - 1) + 0.5f);
    return g_srgb_lut[i];
}

/* Convert a half-float scanout (XRGB16161616F and its BGR/alpha siblings, fourcc
 * 'XR4H' etc., 8 bytes/pixel) to 8-bit XRGB8888. FP16 scanouts carry LINEAR light,
 * so each half is decoded to linear and re-encoded through the sRGB OETF -- unlike
 * the integer 16-bit path, which treats its samples as already sRGB-encoded. HDR
 * values above 1.0 clip to white: a faithful HDR tone-map needs the FP16 colorimetry
 * the caller does not carry here, so this favors a correct, viewable SDR-range
 * result over a speculative remap (and is still far better than the raw 16-bit bytes
 * a consumer would otherwise misread as XRGB8888). */
int drmtap_convert_rgb16f(const void *src, void *dst,
                          uint32_t width, uint32_t height,
                          uint32_t src_stride, uint32_t dst_stride, int bgr) {
    if (!src || !dst || width == 0 || height == 0) {
        return -EINVAL;
    }
    size_t src_row = (size_t)width * 8u;
    size_t dst_row = (size_t)width * 4u;
    if (src_row > UINT32_MAX || dst_row > UINT32_MAX ||
        src_stride < src_row || dst_stride < dst_row) {
        return -EINVAL;
    }
    pthread_once(&g_hdr_once, hdr_lut_init);   /* fills g_srgb_lut */
    /* Memory order of the 16-bit quad is B,G,R,X for the RGB variants (XR4H/AR4H)
     * and R,G,B,X for the BGR variants (XB4H/AB4H) -- identical to the integer
     * drmtap_convert_rgb16 path, so red sits at unit 2 unless bgr. */
    int ri = bgr ? 0 : 2;
    int bi = bgr ? 2 : 0;
    for (uint32_t y = 0; y < height; y++) {
        const uint16_t *s = (const uint16_t *)
            ((const uint8_t *)src + (size_t)y * src_stride);
        uint32_t *d = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_stride);
        for (uint32_t x = 0; x < width; x++) {
            const uint16_t *px = s + (size_t)x * 4;
            uint8_t r8 = srgb8_from_linear(half_to_float(px[ri]));
            uint8_t g8 = srgb8_from_linear(half_to_float(px[1]));
            uint8_t b8 = srgb8_from_linear(half_to_float(px[bi]));
            d[x] = (0xFFu << 24) | ((uint32_t)r8 << 16) |
                   ((uint32_t)g8 << 8) | b8;
        }
    }
    return 0;
}

int drmtap_convert_format(const void *src, void *dst,
                          uint32_t width, uint32_t height,
                          uint32_t src_stride, uint32_t dst_stride,
                          uint32_t src_format, uint32_t dst_format) {
    if (!src || !dst || width == 0 || height == 0) {
        return -EINVAL;
    }
    /* Every format this converter handles (AR30/XR30 source, XRGB8888 dest, and
     * the same-format copy) is 4 bytes/pixel, so a stride narrower than width*4
     * would read or write past each row. Reject like drmtap_convert_rgb16 does. */
    if ((size_t)src_stride < (size_t)width * 4u ||
        (size_t)dst_stride < (size_t)width * 4u) {
        return -EINVAL;
    }

    /* Same format — just copy */
    if (src_format == dst_format) {
        for (uint32_t y = 0; y < height; y++) {
            memcpy((uint8_t *)dst + y * dst_stride,
                   (const uint8_t *)src + y * src_stride,
                   width * 4);
        }
        return 0;
    }

    /*
     * DRM fourcc values (from drm_fourcc.h):
     *   XRGB8888 = 0x34325258  ('XR24')
     *   ARGB8888 = 0x34325241  ('AR24')
     *   ABGR8888 = 0x34324241  ('AB24')
     *   XRGB2101010 = 0x30335258  ('XR30')
     *   ARGB2101010 = 0x30335241  ('AR30')
     */
    #define DRM_FMT_XRGB8888    0x34325258u
    #define DRM_FMT_ARGB8888    0x34325241u
    #define DRM_FMT_ABGR8888    0x34324241u
    #define DRM_FMT_XRGB2101010 0x30335258u
    #define DRM_FMT_ARGB2101010 0x30335241u
    #define DRM_FMT_XBGR2101010 0x30334258u
    #define DRM_FMT_ABGR2101010 0x30334241u

    /* X/AR30 (RGB) and X/AB30 (BGR) 10-bit → XRGB8888/ARGB8888 */
    if ((src_format == DRM_FMT_XRGB2101010 ||
         src_format == DRM_FMT_ARGB2101010 ||
         src_format == DRM_FMT_XBGR2101010 ||
         src_format == DRM_FMT_ABGR2101010) &&
        (dst_format == DRM_FMT_XRGB8888 ||
         dst_format == DRM_FMT_ARGB8888)) {
        int bgr = (src_format == DRM_FMT_XBGR2101010 ||
                   src_format == DRM_FMT_ABGR2101010);
        convert_ar30_to_xrgb8888(src, dst, width, height,
                                 src_stride, dst_stride, bgr);
        return 0;
    }

    /* ABGR8888 → ARGB8888 */
    if (src_format == DRM_FMT_ABGR8888 &&
        (dst_format == DRM_FMT_ARGB8888 ||
         dst_format == DRM_FMT_XRGB8888)) {
        convert_abgr_to_argb(src, dst, width, height,
                             src_stride, dst_stride);
        return 0;
    }

    /* Unsupported conversion */
    return -ENOTSUP;
}

/* ========================================================================= */
/* Public API — Frame Differencing                                           */
/* ========================================================================= */

int drmtap_diff_frames(const void *frame_a, const void *frame_b,
                       uint32_t width, uint32_t height, uint32_t stride,
                       drmtap_rect *rects_out, int max_rects,
                       int tile_size) {
    if (!frame_a || !frame_b || !rects_out || max_rects <= 0 || tile_size <= 0) {
        return -EINVAL;
    }

    int dirty_count = 0;
    uint32_t tiles_x = (width + (uint32_t)tile_size - 1) / (uint32_t)tile_size;
    uint32_t tiles_y = (height + (uint32_t)tile_size - 1) / (uint32_t)tile_size;

    for (uint32_t ty = 0; ty < tiles_y; ty++) {
        for (uint32_t tx = 0; tx < tiles_x; tx++) {
            uint32_t rx = tx * (uint32_t)tile_size;
            uint32_t ry = ty * (uint32_t)tile_size;
            uint32_t rw = (rx + (uint32_t)tile_size > width)
                          ? width - rx : (uint32_t)tile_size;
            uint32_t rh = (ry + (uint32_t)tile_size > height)
                          ? height - ry : (uint32_t)tile_size;

            int dirty = 0;
            for (uint32_t row = ry; row < ry + rh && !dirty; row++) {
                const uint8_t *a = (const uint8_t *)frame_a +
                                   row * stride + rx * 4;
                const uint8_t *b = (const uint8_t *)frame_b +
                                   row * stride + rx * 4;
                if (memcmp(a, b, rw * 4) != 0) {
                    dirty = 1;
                }
            }

            if (dirty) {
                if (dirty_count < max_rects) {
                    rects_out[dirty_count].x = rx;
                    rects_out[dirty_count].y = ry;
                    rects_out[dirty_count].w = rw;
                    rects_out[dirty_count].h = rh;
                }
                dirty_count++;
            }
        }
    }

    return dirty_count;
}
