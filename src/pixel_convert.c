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
                                   uint32_t src_stride, uint32_t dst_stride) {
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

            /* Copy pixel */
            memcpy(d + y * dst_stride + x * bpp,
                   s + src_off, bpp);
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
                                   uint32_t src_stride, uint32_t dst_stride) {
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

            memcpy(d + y * dst_stride + x * bpp,
                   s + src_off, bpp);
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
                                    uint32_t src_stride, uint32_t dst_stride) {
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

            memcpy(d + y * dst_stride + dst_x * bpp,
                   s + src_off, copy_w * bpp);
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
                                     uint32_t dst_stride) {
    for (uint32_t y = 0; y < height; y++) {
        const uint32_t *s = (const uint32_t *)
            ((const uint8_t *)src + y * src_stride);
        uint32_t *d = (uint32_t *)
            ((uint8_t *)dst + y * dst_stride);

        for (uint32_t x = 0; x < width; x++) {
            uint32_t pixel = s[x];
            uint8_t r = (uint8_t)((pixel >> 22) & 0xFF);  /* [29:20] >> 2 */
            uint8_t g = (uint8_t)((pixel >> 12) & 0xFF);  /* [19:10] >> 2 */
            uint8_t b = (uint8_t)((pixel >> 2) & 0xFF);   /* [9:0] >> 2 */
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
                     uint64_t modifier) {
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

    /* Linear — just memcpy row by row */
    if (modifier == 0 /* DRM_FORMAT_MOD_LINEAR */) {
        for (uint32_t y = 0; y < height; y++) {
            memcpy((uint8_t *)dst + y * dst_stride,
                   (const uint8_t *)src + y * src_stride,
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
                                           src_stride, dst_stride);
        } else if (mod_type == 0x02 || mod_type == 0x03) {
            /* I915_FORMAT_MOD_Y_TILED (0x02)
             * I915_FORMAT_MOD_Yf_TILED (0x03)
             *
             * Pure Y-tiled without compression — CPU deswizzle works. */
            return deswizzle_intel_y_tiled(src, dst, width, height,
                                           src_stride, dst_stride);
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
                                         src_stride, dst_stride);
    }

    /* Unknown modifier — try linear copy as fallback */
    for (uint32_t y = 0; y < height; y++) {
        memcpy((uint8_t *)dst + y * dst_stride,
               (const uint8_t *)src + y * src_stride,
               width * 4);
    }
    return 0;
}

/* ========================================================================= */
/* Public API — Format Conversion                                            */
/* ========================================================================= */

int drmtap_convert_format(const void *src, void *dst,
                          uint32_t width, uint32_t height,
                          uint32_t src_stride, uint32_t dst_stride,
                          uint32_t src_format, uint32_t dst_format) {
    if (!src || !dst || width == 0 || height == 0) {
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

    /* AR30/XR30 → XRGB8888/ARGB8888 */
    if ((src_format == DRM_FMT_XRGB2101010 ||
         src_format == DRM_FMT_ARGB2101010) &&
        (dst_format == DRM_FMT_XRGB8888 ||
         dst_format == DRM_FMT_ARGB8888)) {
        convert_ar30_to_xrgb8888(src, dst, width, height,
                                 src_stride, dst_stride);
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
