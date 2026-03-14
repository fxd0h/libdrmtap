/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file test_deswizzle.c
 * @brief Unit test — deswizzle tiled formats and pixel format conversion
 *
 * No hardware needed — uses synthetic tiled data to verify deswizzle
 * algorithms produce correct linear output.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "drmtap.h"

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

/* ========================================================================= */
/* Deswizzle tests                                                           */
/* ========================================================================= */

static void test_deswizzle_linear(void) {
    /* Linear modifier (0) should just copy row-by-row */
    uint32_t w = 64, h = 64;
    uint32_t stride = w * 4;
    uint8_t *src = calloc(1, stride * h);
    uint8_t *dst = calloc(1, stride * h);
    TEST_ASSERT(src && dst);

    /* Fill with pattern */
    for (uint32_t i = 0; i < stride * h; i++) {
        src[i] = (uint8_t)(i & 0xFF);
    }

    int ret = drmtap_deswizzle(src, dst, w, h, stride, stride, 0);
    TEST_ASSERT(ret == 0);
    TEST_ASSERT(memcmp(src, dst, stride * h) == 0);

    free(src);
    free(dst);
    printf("  PASS: deswizzle linear (copy)\n");
}

static void test_deswizzle_null_safety(void) {
    int ret = drmtap_deswizzle(NULL, NULL, 64, 64, 256, 256, 0);
    TEST_ASSERT(ret == -22);  /* -EINVAL */
    printf("  PASS: deswizzle NULL safety\n");
}

static void test_deswizzle_intel_x_tiled_roundtrip(void) {
    /*
     * Intel X-TILED: 512 bytes wide × 8 rows per tile
     * At 32bpp: 128 pixels × 8 rows
     *
     * Create linear data, manually tile it, then deswizzle.
     * Result should match original linear data.
     */
    uint32_t w = 128, h = 16;
    uint32_t stride = w * 4;  /* 512 bytes = exactly 1 tile width */
    uint32_t tile_w = 512;
    uint32_t tile_h = 8;
    uint32_t tiles_x = stride / tile_w;
    uint32_t tiles_y = h / tile_h;

    uint8_t *linear = calloc(1, stride * h);
    uint8_t *tiled = calloc(1, stride * h);
    uint8_t *result = calloc(1, stride * h);
    TEST_ASSERT(linear && tiled && result);

    /* Create linear reference pattern */
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t *p = (uint32_t *)(linear + y * stride + x * 4);
            *p = (y << 16) | (x << 8) | 0x42;  /* encode position */
        }
    }

    /* Manually tile the data (reverse of deswizzle) */
    for (uint32_t y = 0; y < h; y++) {
        uint32_t tile_row = y / tile_h;
        uint32_t tile_y = y % tile_h;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t x_bytes = x * 4;
            uint32_t tile_col = x_bytes / tile_w;
            uint32_t tile_x_bytes = x_bytes % tile_w;
            uint32_t tile_idx = tile_row * tiles_x + tile_col;
            uint32_t tile_size = tile_w * tile_h;
            uint32_t tiled_off = tile_idx * tile_size +
                                 tile_y * tile_w + tile_x_bytes;
            memcpy(tiled + tiled_off, linear + y * stride + x * 4, 4);
        }
    }

    /* Deswizzle: I915_FORMAT_MOD_X_TILED = 0x0100000000000001 */
    uint64_t mod_x_tiled = 0x0100000000000001ULL;
    int ret = drmtap_deswizzle(tiled, result, w, h, stride, stride, mod_x_tiled);
    TEST_ASSERT(ret == 0);

    /* Verify result matches original linear */
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t expected = *((uint32_t *)(linear + y * stride + x * 4));
            uint32_t got = *((uint32_t *)(result + y * stride + x * 4));
            if (expected != got) {
                fprintf(stderr,
                        "FAIL: pixel (%u,%u) expected=0x%08x got=0x%08x\n",
                        x, y, expected, got);
                exit(1);
            }
        }
    }

    (void)tiles_y;

    free(linear);
    free(tiled);
    free(result);
    printf("  PASS: Intel X-TILED roundtrip (128x16)\n");
}

static void test_deswizzle_nvidia_x_tiled_roundtrip(void) {
    /*
     * Nvidia X-TILED: 16 pixels × 128 rows per tile
     * At 32bpp: 64 bytes × 128 rows = 8192 bytes per tile
     */
    uint32_t w = 32, h = 128;
    uint32_t stride = w * 4;  /* 128 bytes */
    uint32_t tile_w = 16;     /* pixels */
    uint32_t tile_h = 128;
    uint32_t tiles_x = (w + tile_w - 1) / tile_w;
    uint32_t tile_w_bytes = tile_w * 4;

    uint8_t *linear = calloc(1, stride * h);
    /* Tiled size: tiles_x tiles × (tile_w_bytes × tile_h) bytes */
    size_t tiled_size = tiles_x * tile_w_bytes * tile_h;
    uint8_t *tiled = calloc(1, tiled_size);
    uint8_t *result = calloc(1, stride * h);
    TEST_ASSERT(linear && tiled && result);

    /* Create linear reference */
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t *p = (uint32_t *)(linear + y * stride + x * 4);
            *p = (y << 16) | (x << 8) | 0x99;
        }
    }

    /* Manually tile (reverse of deswizzle) */
    for (uint32_t y = 0; y < h; y++) {
        uint32_t tile_row = y / tile_h;
        uint32_t tile_y = y % tile_h;
        for (uint32_t tx = 0; tx < tiles_x; tx++) {
            uint32_t tile_idx = tile_row * tiles_x + tx;
            uint32_t src_off = tile_idx * (tile_w_bytes * tile_h) +
                               tile_y * tile_w_bytes;
            uint32_t dst_x = tx * tile_w;
            uint32_t copy_w = (dst_x + tile_w > w) ? w - dst_x : tile_w;
            memcpy(tiled + src_off,
                   linear + y * stride + dst_x * 4, copy_w * 4);
        }
    }

    /* Deswizzle: Nvidia vendor = 0x10 */
    uint64_t mod_nvidia = 0x1000000000000000ULL;
    int ret = drmtap_deswizzle(tiled, result, w, h, stride, stride, mod_nvidia);
    TEST_ASSERT(ret == 0);

    /* Verify */
    TEST_ASSERT(memcmp(linear, result, stride * h) == 0);

    free(linear);
    free(tiled);
    free(result);
    printf("  PASS: Nvidia X-TILED roundtrip (32x128)\n");
}

/* ========================================================================= */
/* Format conversion tests                                                   */
/* ========================================================================= */

static void test_convert_ar30_to_xrgb8888(void) {
    /*
     * AR30 format: [2:alpha][10:red][10:green][10:blue]
     * Test: pure red at max (0x3FF << 20)
     */
    uint32_t w = 4, h = 1;
    uint32_t stride = w * 4;
    uint32_t src[4];
    uint32_t dst[4];

    /* Pure red: alpha=3, R=1023, G=0, B=0 */
    src[0] = (3u << 30) | (1023u << 20) | (0u << 10) | 0u;
    /* Pure green: R=0, G=1023, B=0 */
    src[1] = (3u << 30) | (0u << 20) | (1023u << 10) | 0u;
    /* Pure blue: R=0, G=0, B=1023 */
    src[2] = (3u << 30) | (0u << 20) | (0u << 10) | 1023u;
    /* Mid gray: R=512, G=512, B=512 */
    src[3] = (3u << 30) | (512u << 20) | (512u << 10) | 512u;

    /* XR30 fourcc = 0x30335258 */
    /* XRGB8888 fourcc = 0x34325258 */
    int ret = drmtap_convert_format(src, dst, w, h, stride, stride,
                                    0x30335258u, 0x34325258u);
    TEST_ASSERT(ret == 0);

    /* Check red channel of pixel 0: (1023 >> 2) = 255 */
    uint8_t r0 = (uint8_t)((dst[0] >> 16) & 0xFF);
    TEST_ASSERT(r0 == 255);

    /* Check green channel of pixel 1: (1023 >> 2) = 255 */
    uint8_t g1 = (uint8_t)((dst[1] >> 8) & 0xFF);
    TEST_ASSERT(g1 == 255);

    /* Check blue channel of pixel 2: (1023 >> 2) = 255 */
    uint8_t b2 = (uint8_t)(dst[2] & 0xFF);
    TEST_ASSERT(b2 == 255);

    printf("  PASS: AR30 → XRGB8888 conversion\n");
}

static void test_convert_abgr_to_argb(void) {
    uint32_t w = 2, h = 1;
    uint32_t stride = w * 4;
    uint32_t src[2];
    uint32_t dst[2];

    /* ABGR: [A=0xFF][B=0x11][G=0x22][R=0x33] */
    src[0] = 0xFF112233u;
    src[1] = 0x80AABBCC;

    /* ABGR8888 fourcc = 0x34324241 */
    /* ARGB8888 fourcc = 0x34325241 */
    int ret = drmtap_convert_format(src, dst, w, h, stride, stride,
                                    0x34324241u, 0x34325241u);
    TEST_ASSERT(ret == 0);

    /* ARGB: [A=0xFF][R=0x33][G=0x22][B=0x11] */
    TEST_ASSERT(dst[0] == 0xFF332211u);
    TEST_ASSERT(dst[1] == 0x80CCBB00 + 0xAA);

    printf("  PASS: ABGR8888 → ARGB8888 conversion\n");
}

static void test_convert_same_format(void) {
    uint32_t w = 4, h = 1;
    uint32_t stride = w * 4;
    uint32_t src[4] = {0x11223344, 0x55667788, 0x99AABBCC, 0xDDEEFF00};
    uint32_t dst[4] = {0};

    int ret = drmtap_convert_format(src, dst, w, h, stride, stride,
                                    0x34325258u, 0x34325258u);
    TEST_ASSERT(ret == 0);
    TEST_ASSERT(memcmp(src, dst, sizeof(src)) == 0);

    printf("  PASS: same format copy\n");
}

static void test_convert_unsupported(void) {
    uint32_t src[4] = {0};
    uint32_t dst[4] = {0};
    int ret = drmtap_convert_format(src, dst, 4, 1, 16, 16,
                                    0x12345678u, 0x87654321u);
    TEST_ASSERT(ret == -95);  /* -ENOTSUP */
    printf("  PASS: unsupported format returns -ENOTSUP\n");
}

/* ========================================================================= */
/* Main                                                                      */
/* ========================================================================= */

int main(void) {
    printf("Running deswizzle/conversion tests...\n");
    test_deswizzle_null_safety();
    test_deswizzle_linear();
    test_deswizzle_intel_x_tiled_roundtrip();
    test_deswizzle_nvidia_x_tiled_roundtrip();
    test_convert_ar30_to_xrgb8888();
    test_convert_abgr_to_argb();
    test_convert_same_format();
    test_convert_unsupported();
    printf("All deswizzle/conversion tests passed!\n");
    return 0;
}
