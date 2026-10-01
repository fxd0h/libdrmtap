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

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drmtap.h"

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static void test_version(void) {
    int v = drmtap_version();
    /* Track the header macros so this never goes stale on a version bump. */
    TEST_ASSERT(v == ((DRMTAP_VERSION_MAJOR << 16) |
                      (DRMTAP_VERSION_MINOR << 8) |
                      DRMTAP_VERSION_PATCH));
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

/* The exact refresh, rounded the kernel's way, is the vrefresh the enumeration reports
 * for an active display: the fraction only adds the precision vrefresh drops. */
static void test_crtc_refresh(void) {
    drmtap_ctx *ctx = drmtap_open(NULL);
    TEST_ASSERT(ctx != NULL);
    drmtap_display displays[8];
    int n = drmtap_list_displays(ctx, displays, 8);
    drmtap_close(ctx);

    uint64_t num = 0, den = 0;
    TEST_ASSERT(drmtap_crtc_refresh(NULL, &num, &den) == -EINVAL);

    int checked = 0;
    for (int i = 0; i < n && i < 8; i++) {
        if (!displays[i].active || displays[i].crtc_id == 0) {
            continue;
        }
        drmtap_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.crtc_id = displays[i].crtc_id;
        drmtap_ctx *c = drmtap_open(&cfg);
        TEST_ASSERT(c != NULL);
        TEST_ASSERT(drmtap_crtc_refresh(c, NULL, &den) == -EINVAL);
        int rc = drmtap_crtc_refresh(c, &num, &den);
        drmtap_close(c);
        TEST_ASSERT(rc == 0 && den != 0);
        /* Since Linux 5.9 GETCRTC's vrefresh is the kernel's rounded value. Before it the
         * kernel echoed what userspace set: 0 or a truncated rate under Xorg. */
        uint32_t hz = displays[i].refresh_hz;
        TEST_ASSERT(hz == 0 || (num + den / 2) / den == hz || num / den == hz);
        printf("    [%d] %s: %llu/%llu Hz = %.6f (vrefresh %u)\n", i, displays[i].name,
               (unsigned long long)num, (unsigned long long)den, (double)num / (double)den,
               displays[i].refresh_hz);
        checked++;
    }
    if (checked == 0) {
        printf("  SKIP: crtc_refresh (no active display)\n");
        return;
    }
    /* A crtc_id 0 context picks a CRTC on the first call and keeps it on the next. */
    drmtap_ctx *a = drmtap_open(NULL);
    TEST_ASSERT(a != NULL);
    uint64_t n1 = 0, d1 = 0, n2 = 0, d2 = 0;
    TEST_ASSERT(drmtap_crtc_refresh(a, &n1, &d1) == 0 && d1 != 0);
    TEST_ASSERT(drmtap_crtc_refresh(a, &n2, &d2) == 0 && n2 == n1 && d2 == d1);
    drmtap_close(a);
    printf("  PASS: crtc_refresh on %d display(s)\n", checked);
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
    test_crtc_refresh();
    printf("All enumeration tests passed!\n");
    return 0;
}
