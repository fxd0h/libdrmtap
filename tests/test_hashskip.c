/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_hashskip.c
 * @brief DRMTAP_SKIP_UNCHANGED: what a caller observes through the GPU convert
 *
 * Converts a udmabuf-backed XRGB8888 image through the EGL path with the
 * opt-in set: an untouched image gives the same pixels again, and a change in
 * any block (first, middle, last) shows up in the next frame. SKIPs without
 * /dev/udmabuf, a render node or a working EGL.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "drmtap.h"
#include "udmabuf_util.h"
#define W 1920u /* the hash only runs from 1080p up */
#define H 1080u
#define XR24 0x34325258u
static int failures = 0;
static void check(int cond, const char *what) {
    printf("  %s: %s\n", cond ? "OK" : "FAIL", what);
    failures += !cond;
}
/* Compare ignoring the X byte, which the GPU path reports as 0xff. */
static int same_rgb(const uint32_t *a, const uint32_t *b) {
    for (size_t i = 0; i < (size_t)W * H; i++) {
        if ((a[i] & 0x00ffffffu) != (b[i] & 0x00ffffffu)) return 0;
    }
    return 1;
}
int main(void) {
    printf("test_hashskip: skip-unchanged through the GPU convert\n");
    setenv("DRMTAP_SKIP_UNCHANGED", "1", 0);
    static uint32_t src[W * H], want[W * H];
    for (size_t i = 0; i < (size_t)W * H; i++) src[i] = 0x00010203u * (uint32_t)(i % 251);
    int fd = drmtap_udmabuf_make(src, sizeof(src));
    drmtap_ctx *ctx = fd >= 0 ? drmtap_open_render(NULL) : NULL;
    if (fd < 0 || !ctx) {
        if (ctx) drmtap_close(ctx);
        if (fd >= 0) close(fd);
        printf("  SKIP: no udmabuf / render node\n");
        return 0;
    }
    drmtap_dmabuf_desc d;
    memset(&d, 0, sizeof(d));
    d.dma_buf_fd = fd; d.width = W; d.height = H; d.format = XR24;
    d.num_planes = 1; d.pitches[0] = W * 4; /* fb_id 0: a fresh import per call, as CPU writes
                              * to a udmabuf are not seen through a cached one */
    drmtap_frame_info f;
    memset(&f, 0, sizeof(f));
    memcpy(want, src, sizeof(src));
    if (drmtap_convert_dmabuf(ctx, &d, &f) != 0 || drmtap_frame_owns_data(&f)) {
        if (f.data && drmtap_frame_owns_data(&f)) drmtap_frame_release(ctx, &f);
        drmtap_close(ctx);
        close(fd);
        printf("  SKIP: the GPU path is not in use here\n");
        return 0;
    }
    check(same_rgb(f.data, want), "first frame is the image");
    uint32_t *first = f.data;
    /* The skip is observable: an untouched image is not converted again, so a
     * mark left in the returned buffer survives (it is gone with the feature off). */
    first[0] = 0xdeadbeefu;
    check(drmtap_convert_dmabuf(ctx, &d, &f) == 0 && f.data == first &&
          first[0] == 0xdeadbeefu, "untouched image: the convert is skipped");
    first[0] = want[0];
    for (int i = 0; i < 3; i++) {
        check(drmtap_convert_dmabuf(ctx, &d, &f) == 0 && f.data == first &&
              same_rgb(f.data, want), "untouched image: same buffer, same pixels");
    }
    static const size_t pos[] = { 0, (size_t)(H / 2) * W + 100, (size_t)W * H - 1 };
    for (int i = 0; i < 3; i++) {
        want[pos[i]] ^= 0x00ffffffu;
        /* A new dma-buf with the changed pixel: the GPU does not see CPU
         * writes made through an old udmabuf mapping, on this driver or with
         * the feature off, so a stale read here would be the box, not us. */
        memcpy(src, want, sizeof(src));
        close(fd);
        fd = drmtap_udmabuf_make(src, sizeof(src));
        d.dma_buf_fd = fd;
        check(drmtap_convert_dmabuf(ctx, &d, &f) == 0 && same_rgb(f.data, want),
              "one changed pixel shows in the next frame");
        check(drmtap_convert_dmabuf(ctx, &d, &f) == 0 && same_rgb(f.data, want),
              "and stays after an unchanged frame");
    }
    drmtap_close(ctx);
    close(fd);
    printf("test_hashskip: %d failures\n", failures);
    return failures ? 1 : 0;
}
