/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file test_formats.c
 * @brief Unit test — pixel format conversion and frame differencing
 *
 * No hardware needed — works with synthetic data.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "drmtap.h"

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static void test_diff_identical_frames(void) {
    /* Two identical 64x64 frames → 0 dirty rects */
    uint32_t w = 64, h = 64, stride = w * 4;
    uint8_t *frame = calloc(1, stride * h);
    TEST_ASSERT(frame != NULL);
    memset(frame, 0xAB, stride * h);

    drmtap_rect rects[16];
    int n = drmtap_diff_frames(frame, frame, w, h, stride, rects, 16, 32);
    TEST_ASSERT(n == 0);

    free(frame);
    printf("  PASS: identical frames → 0 dirty rects\n");
}

static void test_diff_completely_different(void) {
    /* Two fully different 64x64 frames → all tiles dirty */
    uint32_t w = 64, h = 64, stride = w * 4;
    uint8_t *frame_a = calloc(1, stride * h);
    uint8_t *frame_b = calloc(1, stride * h);
    TEST_ASSERT(frame_a != NULL && frame_b != NULL);

    memset(frame_a, 0x00, stride * h);
    memset(frame_b, 0xFF, stride * h);

    drmtap_rect rects[16];
    int n = drmtap_diff_frames(frame_a, frame_b, w, h, stride, rects, 16, 32);
    /* 64/32 = 2 tiles per axis = 4 tiles total */
    TEST_ASSERT(n == 4);

    free(frame_a);
    free(frame_b);
    printf("  PASS: completely different → 4 dirty rects (2x2 tiles)\n");
}

static void test_diff_single_pixel_change(void) {
    /* Change one pixel → exactly 1 dirty tile */
    uint32_t w = 128, h = 128, stride = w * 4;
    uint8_t *frame_a = calloc(1, stride * h);
    uint8_t *frame_b = calloc(1, stride * h);
    TEST_ASSERT(frame_a != NULL && frame_b != NULL);

    memcpy(frame_b, frame_a, stride * h);
    frame_b[0] = 0xFF;  /* change pixel (0,0) */

    drmtap_rect rects[64];
    int n = drmtap_diff_frames(frame_a, frame_b, w, h, stride, rects, 64, 64);
    TEST_ASSERT(n == 1);
    TEST_ASSERT(rects[0].x == 0 && rects[0].y == 0);
    TEST_ASSERT(rects[0].w == 64 && rects[0].h == 64);

    free(frame_a);
    free(frame_b);
    printf("  PASS: single pixel change → 1 dirty rect at (0,0)\n");
}

static void test_diff_null_safety(void) {
    drmtap_rect rects[4];
    int ret = drmtap_diff_frames(NULL, NULL, 64, 64, 256, rects, 4, 32);
    TEST_ASSERT(ret == -22);
    printf("  PASS: diff_frames NULL safety\n");
}

static void test_diff_overflow_rects(void) {
    /* More dirty tiles than max_rects → returns total count */
    uint32_t w = 128, h = 128, stride = w * 4;
    uint8_t *frame_a = calloc(1, stride * h);
    uint8_t *frame_b = calloc(1, stride * h);
    TEST_ASSERT(frame_a != NULL && frame_b != NULL);
    memset(frame_b, 0xFF, stride * h);

    drmtap_rect rects[2];  /* only space for 2 */
    int n = drmtap_diff_frames(frame_a, frame_b, w, h, stride, rects, 2, 32);
    /* 128/32 = 4 tiles per axis = 16 total, but only 2 fit */
    TEST_ASSERT(n == 16);

    free(frame_a);
    free(frame_b);
    printf("  PASS: overflow → returns total count (%d) > max_rects (2)\n", n);
}

/* drmtap_deswizzle must FAIL CLOSED on any layout it cannot decode.
 *
 * This is pinned because the opposite shipped: an unrecognised modifier fell
 * through to a linear memcpy and returned 0, so a caller received a still-tiled
 * buffer relabelled linear and reported as a valid frame. Measured on a Meteor
 * Lake scanout (modifier 0x10000000000000f) it was a full screen of tile noise,
 * byte-identical run to run and 99.7% different from what the EGL detile of the
 * same framebuffer produced. Returning an error instead is what lets a consumer
 * fall back (rustdesk demotes to PipeWire) rather than show garbage.
 *
 * Pure, so every modifier is covered here regardless of the GPU in this machine
 * -- which is the whole reason the gap survived: the box that would have caught
 * it had a GPU whose modifier happened to be in the supported list. */
#define MOD_INTEL(v)  (0x0100000000000000ULL | (v))
#define MOD_INVALID   0x00ffffffffffffffULL

static void test_deswizzle_fails_closed_on_undecodable_layouts(void) {
    const uint32_t w = 64, h = 64, stride = w * 4;
    const size_t size = (size_t)stride * h;
    uint8_t *src = calloc(1, size);
    uint8_t *dst = calloc(1, size);
    TEST_ASSERT(src != NULL && dst != NULL);

    /* Decodable: linear, the unstated layout, and the plain tilings. */
    TEST_ASSERT(drmtap_deswizzle(src, dst, w, h, stride, stride, 0, size) == 0);
    TEST_ASSERT(drmtap_deswizzle(src, dst, w, h, stride, stride, MOD_INVALID, size) == 0);
    for (uint64_t v = 0x01; v <= 0x03; v++) {
        TEST_ASSERT(drmtap_deswizzle(src, dst, w, h, stride, stride,
                                     MOD_INTEL(v), size) == 0);
    }

    /* Not decodable: 0x04..0x08 are the Y-tiled CCS variants, 0x09 is 4_TILED,
     * and 0x0a..0x11 are the Tile4 CCS families (DG2, MTL, LNL, BMG). Every one
     * of these except 0x05..0x08 used to return 0 with garbage in dst. */
    for (uint64_t v = 0x04; v <= 0x11; v++) {
        int ret = drmtap_deswizzle(src, dst, w, h, stride, stride,
                                   MOD_INTEL(v), size);
        if (ret != -ENOTSUP) {
            fprintf(stderr, "FAIL: %s:%d: Intel modifier 0x%llx returned %d, "
                    "expected -ENOTSUP\n", __FILE__, __LINE__,
                    (unsigned long long)MOD_INTEL(v), ret);
            exit(1);
        }
    }

    /* A vendor whose tiling we do not decode at all (AMD = 0x02). */
    TEST_ASSERT(drmtap_deswizzle(src, dst, w, h, stride, stride,
                                 0x0200000004801a01ULL, size) == -ENOTSUP);

    free(src);
    free(dst);
    printf("  PASS: undecodable modifiers fail closed, decodable ones still work\n");
}

int main(void) {
    printf("Running format/diff tests...\n");
    test_diff_null_safety();
    test_diff_identical_frames();
    test_diff_completely_different();
    test_diff_single_pixel_change();
    test_diff_overflow_rects();
    test_deswizzle_fails_closed_on_undecodable_layouts();
    printf("All format tests passed!\n");
    return 0;
}
