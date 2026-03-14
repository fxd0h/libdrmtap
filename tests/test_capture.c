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
 * Requires: DRM_DEVICE env var and an active display/compositor
 */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include "drmtap.h"

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static void test_grab_returns_enosys_stub(void) {
    /* While grab is a stub, it should return -ENOSYS */
    drmtap_ctx *ctx = drmtap_open(NULL);
    TEST_ASSERT(ctx != NULL);

    drmtap_frame_info frame = {0};
    int ret = drmtap_grab_mapped(ctx, &frame);
    TEST_ASSERT(ret == -ENOSYS);

    drmtap_close(ctx);
    printf("  PASS: grab_mapped returns -ENOSYS (stub)\n");
}

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

int main(void) {
    printf("Running capture tests...\n");
    test_grab_null_safety();
    test_frame_release_null_safety();
    test_grab_returns_enosys_stub();
    printf("All capture tests passed!\n");
    return 0;
}
