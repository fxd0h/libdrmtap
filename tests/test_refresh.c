/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file test_refresh.c
 * @brief Unit test — the exact refresh of a mode, behind drmtap_crtc_refresh()
 *
 * No hardware needed: the mode -> fraction step is pure. Each mode is a real
 * timing (CEA-861 and the two programmed on a test box), with the fraction it
 * must give and the whole-hertz vrefresh the kernel reports for it.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xf86drmMode.h>

#include "drmtap_internal.h"

static int failures = 0;

static drmModeModeInfo mode_of(uint32_t clock, uint16_t htotal, uint16_t vtotal,
                               uint32_t flags, uint16_t vscan) {
    drmModeModeInfo m;
    memset(&m, 0, sizeof(m));
    m.clock = clock;
    m.htotal = htotal;
    m.vtotal = vtotal;
    m.flags = flags;
    m.vscan = vscan;
    return m;
}

static void expect(const char *what, drmModeModeInfo m, uint64_t num, uint64_t den,
                   uint32_t kernel_hz) {
    uint64_t n = 0, d = 0;
    int rc = drmtap_mode_refresh(&m, &n, &d);
    if (rc != 0 || n != num || d != den) {
        fprintf(stderr, "FAIL: %s -> rc %d, %llu/%llu, expected %llu/%llu\n", what, rc,
                (unsigned long long)n, (unsigned long long)d,
                (unsigned long long)num, (unsigned long long)den);
        failures++;
        return;
    }
    /* Rounded the kernel's way it is still vrefresh: the fraction only adds precision. */
    uint32_t whole = (uint32_t)((n + d / 2) / d);
    if (whole != kernel_hz) {
        fprintf(stderr, "FAIL: %s -> rounds to %u Hz, kernel vrefresh is %u\n", what, whole,
                kernel_hz);
        failures++;
    }
}

int main(void) {
    expect("1080p60", mode_of(148500, 2200, 1125, 0, 0), 60, 1, 60);
    expect("1080p59.94", mode_of(148352, 2200, 1125, 0, 0), 148352, 2475, 60);
    expect("1080p50", mode_of(148500, 2640, 1125, 0, 0), 50, 1, 50);
    expect("1080p29.97", mode_of(74176, 2200, 1125, 0, 0), 74176, 2475, 30);
    expect("1080p23.976", mode_of(74176, 2750, 1125, 0, 0), 296704, 12375, 24);
    expect("1080p119.88", mode_of(296703, 2200, 1125, 0, 0), 2997, 25, 120);
    expect("1080i60, field rate", mode_of(74250, 2200, 1125, DRM_MODE_FLAG_INTERLACE, 0), 60, 1, 60);
    expect("640x480 doublescan", mode_of(25175, 800, 525, DRM_MODE_FLAG_DBLSCAN, 0), 5035, 168, 30);
    expect("640x480 vscan 2", mode_of(25175, 800, 525, 0, 2), 5035, 168, 30);
    expect("vscan 1 is no scan", mode_of(148500, 2200, 1125, 0, 1), 60, 1, 60);
    expect("1280x1024 at 60.02", mode_of(108000, 1688, 1066, 0, 0), 6750000, 112463, 60);
    expect("3840x2160 at 29.98", mode_of(262750, 4000, 2191, 0, 0), 131375, 4382, 30);

    /* A mode without timings has no refresh, and the outputs stay untouched. */
    const drmModeModeInfo empty[] = {
        mode_of(0, 2200, 1125, 0, 0), mode_of(148500, 0, 1125, 0, 0), mode_of(148500, 2200, 0, 0, 0),
    };
    for (size_t i = 0; i < sizeof(empty) / sizeof(empty[0]); i++) {
        uint64_t n = 7, d = 7;
        if (drmtap_mode_refresh(&empty[i], &n, &d) != -EINVAL || n != 7 || d != 7) {
            fprintf(stderr, "FAIL: empty mode %zu not refused\n", i);
            failures++;
        }
    }
    uint64_t n = 0, d = 0;
    drmModeModeInfo ok = mode_of(148500, 2200, 1125, 0, 0);
    if (drmtap_mode_refresh(NULL, &n, &d) != -EINVAL || drmtap_mode_refresh(&ok, NULL, &d) != -EINVAL ||
        drmtap_mode_refresh(&ok, &n, NULL) != -EINVAL) {
        fprintf(stderr, "FAIL: a NULL argument not refused\n");
        failures++;
    }

    /* The widest timings the fields allow do not overflow: 2 x 4294967295 kHz over
     * 65535 x 65535 x 65535, reduced. */
    drmModeModeInfo widest = mode_of(UINT32_MAX, UINT16_MAX, UINT16_MAX, DRM_MODE_FLAG_INTERLACE, UINT16_MAX);
    if (drmtap_mode_refresh(&widest, &n, &d) != 0 || n != 5242960u || d != 171793449u) {
        fprintf(stderr, "FAIL: widest timings -> %llu/%llu\n", (unsigned long long)n, (unsigned long long)d);
        failures++;
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("refresh: all passed\n");
    return 0;
}
