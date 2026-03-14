/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file test_enumerate.c
 * @brief Integration test — enumerate displays using vkms or real GPU
 *
 * Requires: DRM_DEVICE env var (e.g., /dev/dri/card1 for vkms)
 * Run with: DRM_DEVICE=/dev/dri/card1 ./test_enumerate
 */

#include <stdio.h>
#include <stdlib.h>

#include "drmtap.h"

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static void test_version(void) {
    int v = drmtap_version();
    TEST_ASSERT(v == ((0 << 16) | (1 << 8) | 0));
    printf("  PASS: version = 0x%06x\n", v);
}

static void test_open_close(void) {
    drmtap_ctx *ctx = drmtap_open(NULL);
    TEST_ASSERT(ctx != NULL);
    drmtap_close(ctx);
    printf("  PASS: open/close with defaults\n");
}

static void test_open_with_config(void) {
    const char *dev = getenv("DRM_DEVICE");
    if (!dev) {
        printf("  SKIP: test_open_with_config (DRM_DEVICE not set)\n");
        return;
    }

    drmtap_config cfg = {0};
    cfg.device_path = dev;
    cfg.debug = 1;

    drmtap_ctx *ctx = drmtap_open(&cfg);
    TEST_ASSERT(ctx != NULL);

    const char *driver = drmtap_gpu_driver(ctx);
    if (driver) {
        printf("  driver: %s\n", driver);
    }

    drmtap_close(ctx);
    printf("  PASS: open/close with device=%s\n", dev);
}

static void test_list_displays(void) {
    drmtap_ctx *ctx = drmtap_open(NULL);
    TEST_ASSERT(ctx != NULL);

    drmtap_display displays[8];
    int n = drmtap_list_displays(ctx, displays, 8);
    printf("  displays found: %d\n", n);

    for (int i = 0; i < n && i < 8; i++) {
        printf("    [%d] %s: %ux%u@%uHz (crtc=%u, active=%d)\n",
               i, displays[i].name,
               displays[i].width, displays[i].height,
               displays[i].refresh_hz,
               displays[i].crtc_id, displays[i].active);
    }

    drmtap_close(ctx);
    printf("  PASS: list_displays\n");
}

static void test_close_null(void) {
    drmtap_close(NULL);   /* must not crash */
    printf("  PASS: close(NULL) safe\n");
}

static void test_error_null(void) {
    const char *err = drmtap_error(NULL);
    /* May be NULL or a string — both are valid */
    (void)err;
    printf("  PASS: error(NULL) safe\n");
}

int main(void) {
    printf("Running enumeration tests...\n");
    test_version();
    test_open_close();
    test_open_with_config();
    test_close_null();
    test_error_null();
    test_list_displays();
    printf("All enumeration tests passed!\n");
    return 0;
}
