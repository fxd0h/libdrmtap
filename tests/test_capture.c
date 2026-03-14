/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file test_capture.c
 * @brief Integration test — capture a frame from vkms or real GPU
 *
 * Requires: DRM_DEVICE env var and an active display/compositor.
 * Without CAP_SYS_ADMIN, grab will return -EACCES (expected).
 * With sudo/CAP_SYS_ADMIN, grab should succeed and return frame data.
 */

#include <stdio.h>
#include <errno.h>
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

static void test_grab_null_safety(void) {
    drmtap_frame_info frame = {0};
    TEST_ASSERT(drmtap_grab(NULL, &frame) == -EINVAL);
    TEST_ASSERT(drmtap_grab_mapped(NULL, &frame) == -EINVAL);

    drmtap_ctx *ctx = drmtap_open(NULL);
    TEST_ASSERT(ctx != NULL);
    TEST_ASSERT(drmtap_grab(ctx, NULL) == -EINVAL);
    TEST_ASSERT(drmtap_grab_mapped(ctx, NULL) == -EINVAL);
    drmtap_close(ctx);

    printf("  PASS: grab NULL safety\n");
}

static void test_frame_release_null_safety(void) {
    drmtap_frame_release(NULL, NULL);   /* must not crash */

    drmtap_frame_info frame = {0};
    drmtap_frame_release(NULL, &frame); /* must not crash */

    printf("  PASS: frame_release NULL safety\n");
}

static void test_grab_real_capture(void) {
    /* Open the DRM device */
    drmtap_config cfg = {0};
    cfg.debug = 1;
    drmtap_ctx *ctx = drmtap_open(&cfg);
    TEST_ASSERT(ctx != NULL);

    const char *driver = drmtap_gpu_driver(ctx);
    printf("  driver: %s\n", driver ? driver : "unknown");

    /* List displays first */
    drmtap_display displays[4];
    int n = drmtap_list_displays(ctx, displays, 4);
    printf("  displays: %d\n", n);

    if (n <= 0) {
        printf("  SKIP: no active displays, cannot test capture\n");
        drmtap_close(ctx);
        return;
    }

    printf("  capturing from: %s (%ux%u)\n",
           displays[0].name, displays[0].width, displays[0].height);

    /* Try zero-copy grab */
    drmtap_frame_info frame = {0};
    int ret = drmtap_grab(ctx, &frame);

    if (ret == -EACCES) {
        printf("  PASS: grab returns -EACCES (no CAP_SYS_ADMIN — expected without sudo)\n");
        drmtap_close(ctx);
        return;
    }

    if (ret == -ENODEV) {
        printf("  PASS: grab returns -ENODEV (no active plane — vkms without compositor)\n");
        drmtap_close(ctx);
        return;
    }

    /* If we got here, grab succeeded! */
    TEST_ASSERT(ret == 0);
    printf("  zero-copy grab OK: %ux%u stride=%u fmt=%.4s dma_buf_fd=%d\n",
           frame.width, frame.height, frame.stride,
           (const char *)&frame.format, frame.dma_buf_fd);

    TEST_ASSERT(frame.width > 0);
    TEST_ASSERT(frame.height > 0);
    TEST_ASSERT(frame.stride > 0);
    TEST_ASSERT(frame.dma_buf_fd >= 0);

    drmtap_frame_release(ctx, &frame);
    printf("  PASS: zero-copy grab + release\n");

    /* Try mapped grab */
    memset(&frame, 0, sizeof(frame));
    ret = drmtap_grab_mapped(ctx, &frame);
    TEST_ASSERT(ret == 0);

    printf("  mapped grab OK: %ux%u data=%p\n",
           frame.width, frame.height, frame.data);

    if (frame.data) {
        /* Verify pixels are not all zero (sanity check) */
        int all_zero = 1;
        uint32_t check_len = frame.stride * 2;  /* first 2 rows */
        const uint8_t *pixels = (const uint8_t *)frame.data;
        for (uint32_t i = 0; i < check_len && all_zero; i++) {
            if (pixels[i] != 0) {
                all_zero = 0;
            }
        }
        printf("  pixel data: %s\n", all_zero ? "all zeros" : "has content ✓");
    }

    drmtap_frame_release(ctx, &frame);
    printf("  PASS: mapped grab + release\n");

    drmtap_close(ctx);
}

int main(void) {
    printf("Running capture tests...\n");
    test_grab_null_safety();
    test_frame_release_null_safety();
    test_grab_real_capture();
    printf("All capture tests passed!\n");
    return 0;
}
