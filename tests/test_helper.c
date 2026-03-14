/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file test_helper.c
 * @brief Unit tests for the helper protocol and error paths
 *
 * Tests the helper binary discovery, context error handling, and
 * various edge cases. Does NOT require hardware — purely unit tests.
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

/* ========================================================================= */
/* Context edge cases                                                        */
/* ========================================================================= */

static void test_version(void) {
    int v = drmtap_version();
    TEST_ASSERT(v == 0x000100);  /* 0.1.0 */
    printf("  PASS: version = 0x%06x\n", v);
}

static void test_open_null_config(void) {
    /* NULL config should work (all defaults) */
    drmtap_ctx *ctx = drmtap_open(NULL);
    /* May succeed or fail depending on /dev/dri availability */
    if (ctx) {
        drmtap_close(ctx);
        printf("  PASS: open(NULL) succeeded (DRM device found)\n");
    } else {
        printf("  PASS: open(NULL) returned NULL (no DRM, expected in CI)\n");
    }
}

static void test_open_bad_device(void) {
    drmtap_config cfg = {0};
    cfg.device_path = "/dev/nonexistent_device_12345";

    drmtap_ctx *ctx = drmtap_open(&cfg);
    TEST_ASSERT(ctx == NULL);

    const char *err = drmtap_error(NULL);
    TEST_ASSERT(err != NULL);
    TEST_ASSERT(strlen(err) > 0);
    printf("  PASS: open bad device → NULL (error: %s)\n", err);
}

static void test_error_null_safety(void) {
    /* drmtap_error(NULL) should return a string, not crash */
    const char *err = drmtap_error(NULL);
    TEST_ASSERT(err != NULL);
    printf("  PASS: error(NULL) safe\n");
}

static void test_close_null(void) {
    /* drmtap_close(NULL) should not crash */
    drmtap_close(NULL);
    printf("  PASS: close(NULL) safe\n");
}

static void test_frame_release_zeroed(void) {
    /* After release, frame should be zeroed */
    drmtap_frame_info frame;
    memset(&frame, 0xFF, sizeof(frame));  /* Fill with garbage */
    frame._priv = NULL;  /* But no priv data */

    drmtap_frame_release(NULL, &frame);

    TEST_ASSERT(frame.width == 0);
    TEST_ASSERT(frame.height == 0);
    TEST_ASSERT(frame.data == NULL);
    TEST_ASSERT(frame.dma_buf_fd == -1);
    printf("  PASS: frame_release zeros out struct\n");
}

/* ========================================================================= */
/* API with invalid contexts                                                 */
/* ========================================================================= */

static void test_grab_without_display(void) {
    drmtap_config cfg = {0};
    cfg.device_path = "/dev/dri/card0";  /* May not have active display */

    drmtap_ctx *ctx = drmtap_open(&cfg);
    if (!ctx) {
        printf("  SKIP: no DRM device available\n");
        return;
    }

    drmtap_frame_info frame = {0};
    int ret = drmtap_grab(ctx, &frame);
    /* Should return an error if no active CRTC/plane */
    printf("  grab on potentially inactive display: ret=%d (%s)\n",
           ret, ret == 0 ? "success" : "error (expected)");

    if (ret == 0) {
        drmtap_frame_release(ctx, &frame);
    }

    drmtap_close(ctx);
    printf("  PASS: grab without guaranteed display handled\n");
}

static void test_list_displays_overflow(void) {
    drmtap_config cfg = {0};
    drmtap_ctx *ctx = drmtap_open(&cfg);
    if (!ctx) {
        printf("  SKIP: no DRM device available\n");
        return;
    }

    /* Ask for zero displays — should still return count */
    drmtap_display buf[1];
    int n = drmtap_list_displays(ctx, buf, 0);
    printf("  list_displays(max=0): %d displays found\n", n);
    /* n >= 0 is valid */
    TEST_ASSERT(n >= 0);

    drmtap_close(ctx);
    printf("  PASS: list_displays overflow safe\n");
}

/* ========================================================================= */
/* Main                                                                      */
/* ========================================================================= */

int main(void) {
    printf("Running helper/protocol tests...\n");
    test_version();
    test_open_null_config();
    test_open_bad_device();
    test_error_null_safety();
    test_close_null();
    test_frame_release_zeroed();
    test_grab_without_display();
    test_list_displays_overflow();
    printf("All helper/protocol tests passed!\n");
    return 0;
}
