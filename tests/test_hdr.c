/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file test_hdr.c
 * @brief Unit test — HDR10 (PQ/BT.2020) -> SDR tone mapping.
 *
 * No HDR hardware needed: we PQ-encode known absolute luminances (nits) into an
 * AR30 buffer, run drmtap_tonemap_hdr10(), and check the resulting 8-bit curve
 * is sane — SDR diffuse white stays bright (the bug the naive >>2 shift caused),
 * the mapping is monotonic, dark stays dark, and peak highlights reach white.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>

#include "drmtap.h"
#include "drmtap_internal.h"  /* drmtap_convert_rgb16, DRMTAP_EOTF_* */

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

/* Inverse PQ (nits -> normalized code [0,1]), to build known HDR inputs. */
static double pq_inverse(double nits) {
    const double m1 = 2610.0 / 16384.0;
    const double m2 = 2523.0 / 4096.0 * 128.0;
    const double c1 = 3424.0 / 4096.0;
    const double c2 = 2413.0 / 4096.0 * 32.0;
    const double c3 = 2392.0 / 4096.0 * 32.0;
    double Lp = nits / 10000.0;
    if (Lp < 0.0) Lp = 0.0;
    double Lm = pow(Lp, m1);
    return pow((c1 + c2 * Lm) / (1.0 + c3 * Lm), m2);
}

/* One gray AR30 pixel (R=G=B) at the given absolute luminance. */
static uint32_t ar30_gray(double nits) {
    double e = pq_inverse(nits);
    uint32_t c = (uint32_t)(e * 1023.0 + 0.5);
    if (c > 1023) c = 1023;
    return (c << 20) | (c << 10) | c;
}

/* Tone-map a gray HDR pixel of `nits` and return its 8-bit value. */
static int map_nits(double nits) {
    uint32_t in = ar30_gray(nits);
    uint32_t out = 0;
    int r = drmtap_tonemap_hdr10(&in, &out, 1, 1, 4, 4, 0x30335258u, 0);
    TEST_ASSERT(r == 0);
    return (int)((out >> 16) & 0xFF); /* gray -> all channels equal */
}

static void test_curve(void) {
    int n5    = map_nits(5);
    int n100  = map_nits(100);
    int n203  = map_nits(203);   /* BT.2408 SDR diffuse white */
    int n600  = map_nits(600);
    int n1000 = map_nits(1000);
    printf("  nits->8bit: 5=%d 100=%d 203(SDR white)=%d 600=%d 1000=%d\n",
           n5, n100, n203, n600, n1000);

    /* Monotonic non-decreasing. */
    TEST_ASSERT(n5 <= n100 && n100 <= n203 && n203 <= n600 && n600 <= n1000);
    /* The fix: SDR diffuse white maps bright, not crushed to mid-gray like the
     * naive top-8-of-10-bits shift did. */
    TEST_ASSERT(n203 >= 230);
    /* Dark stays dark. */
    TEST_ASSERT(n5 < 60);
    /* Peak highlight reaches white. */
    TEST_ASSERT(n1000 >= 250);
    printf("  PASS: tone-map curve sane (SDR white bright, highlights -> white)\n");
}

static void test_unsupported(void) {
    uint32_t a = 0, b = 0;
    /* XR24 (8-bit XRGB8888) is not an HDR source -> -ENOTSUP. */
    int r = drmtap_tonemap_hdr10(&a, &b, 1, 1, 4, 4, 0x34325258u, 0);
    TEST_ASSERT(r == -ENOTSUP);
    /* Bad args. */
    TEST_ASSERT(drmtap_tonemap_hdr10(NULL, &b, 1, 1, 4, 4, 0x30335258u, 0) == -EINVAL);
    /* Strides that cannot hold a full row (2 px need 8 bytes). */
    uint32_t row[2] = {0, 0};
    TEST_ASSERT(drmtap_tonemap_hdr10(row, row, 2, 1, 4, 8, 0x30335258u, 0) == -EINVAL);
    TEST_ASSERT(drmtap_tonemap_hdr10(row, row, 2, 1, 8, 4, 0x30335258u, 0) == -EINVAL);
    printf("  PASS: unsupported format / bad args / bad stride rejected\n");
}

/* One gray XR48 pixel (16-bit, memory order B,G,R,X) at the given luminance. */
static void xr48_gray(double nits, uint16_t px[4]) {
    double e = pq_inverse(nits);
    uint16_t c = (uint16_t)(e * 65535.0 + 0.5);
    px[0] = c; px[1] = c; px[2] = c; px[3] = 0xFFFF;
}

static int map_nits_rgb16(double nits) {
    uint16_t px[4];
    xr48_gray(nits, px);
    uint32_t out = 0;
    int r = drmtap_convert_rgb16(px, &out, 1, 1, 8, 4, 0 /*RGB*/,
                                 DRMTAP_EOTF_PQ, 0);
    TEST_ASSERT(r == 0);
    return (int)((out >> 16) & 0xFF);
}

static void test_rgb16(void) {
    int n5 = map_nits_rgb16(5), n203 = map_nits_rgb16(203),
        n1000 = map_nits_rgb16(1000);
    printf("  rgb16 PQ nits->8bit: 5=%d 203=%d 1000=%d\n", n5, n203, n1000);
    /* Same shape as the AR30 curve: monotonic, SDR white bright, peak -> white. */
    TEST_ASSERT(n5 <= n203 && n203 <= n1000);
    TEST_ASSERT(n5 < 60 && n203 >= 230 && n1000 >= 250);

    /* SDR 16-bit (eotf=SDR): plain high-byte reduction, no tone-map. */
    uint16_t mid[4] = {0x8000, 0x8000, 0x8000, 0xFFFF};
    uint32_t out = 0;
    TEST_ASSERT(drmtap_convert_rgb16(mid, &out, 1, 1, 8, 4, 0,
                                     DRMTAP_EOTF_SDR, 0) == 0);
    TEST_ASSERT(((out >> 16) & 0xFF) == 0x80);  /* 0x8000 >> 8 */

    /* Stride too small (2 px need 16 source bytes). */
    TEST_ASSERT(drmtap_convert_rgb16(mid, &out, 2, 1, 8, 4, 0,
                                     DRMTAP_EOTF_SDR, 0) == -EINVAL);
    printf("  PASS: rgb16 HDR tone-map + SDR reduce\n");
}

int main(void) {
    printf("Running HDR tone-map tests...\n");
    test_curve();
    test_rgb16();
    test_unsupported();
    printf("All HDR tests passed!\n");
    return 0;
}
