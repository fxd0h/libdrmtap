/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file test_convert.c
 * @brief Unit tests for the split-capture convert API
 *        (drmtap_open_render + drmtap_convert_dmabuf)
 *
 * Runs without scanout hardware: argument validation runs everywhere; the
 * pixel tests feed a memfd standing in for a linear DMA-BUF, which exercises
 * the CPU fallback path deterministically (an EGL import of a non-DMA-BUF fd
 * fails and the conversion falls back, and machines without EGL never try
 * it). Prints SKIP and exits clean when no render node / DRM device exists.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>

#include "drmtap.h"

static int failures = 0;

static void check(int cond, const char *what) {
    if (cond) {
        printf("  OK: %s\n", what);
    } else {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

/* DRM fourccs (avoid a libdrm header dependency in a unit test) */
#define FMT_XR24 0x34325258u /* XRGB8888 */
#define FMT_XR30 0x30335258u /* XRGB2101010 */

/* A memfd filled with `size` bytes of `data` stands in for a linear DMA-BUF:
 * the library mmaps it PROT_READ/MAP_SHARED exactly like a real fd, and the
 * DMA_BUF_IOCTL_SYNC calls fail silently (they are best-effort). */
static int make_pixel_memfd(const void *data, size_t size) {
    int fd = memfd_create("drmtap-convert-test", MFD_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (write(fd, data, size) != (ssize_t)size) {
        close(fd);
        return -1;
    }
    return fd;
}

static void test_null_args(void) {
    printf("test_null_args:\n");
    drmtap_dmabuf_desc desc;
    drmtap_frame_info frame;
    memset(&desc, 0, sizeof(desc));
    check(drmtap_convert_dmabuf(NULL, &desc, &frame) == -EINVAL,
          "NULL ctx rejected");
}

static void test_desc_validation(drmtap_ctx *ctx) {
    printf("test_desc_validation:\n");
    drmtap_dmabuf_desc desc;
    drmtap_frame_info frame;

    memset(&desc, 0, sizeof(desc));
    desc.dma_buf_fd = -1;
    desc.width = 64;
    desc.height = 32;
    /* pitches[0] == 0 */
    check(drmtap_convert_dmabuf(ctx, &desc, &frame) == -EINVAL,
          "zero stride rejected");

    desc.pitches[0] = 64 * 4;
    desc.num_planes = 5;
    check(drmtap_convert_dmabuf(ctx, &desc, &frame) == -EINVAL,
          "num_planes > 4 rejected");

    desc.num_planes = 1;
    desc.height = 0x20000000u; /* stride*height far past DRMTAP's FB cap */
    check(drmtap_convert_dmabuf(ctx, &desc, &frame) == -EFBIG,
          "oversized geometry rejected");

    desc.height = 32;
    desc.fb_id = 12345; /* never imported by this context */
    desc.dma_buf_fd = -1;
    check(drmtap_convert_dmabuf(ctx, &desc, &frame) == -EINVAL,
          "unknown fb_id with fd=-1 rejected");

    /* Untrusted descriptor: a stride that does not cover width*bpp must be
     * rejected before any per-pixel converter can read/write past the row. */
    memset(&desc, 0, sizeof(desc));
    desc.dma_buf_fd = -1;
    desc.width = 4096;
    desc.height = 64;
    desc.format = FMT_XR24;
    desc.num_planes = 1;
    desc.pitches[0] = 256; /* only 64 px wide, but width says 4096 */
    check(drmtap_convert_dmabuf(ctx, &desc, &frame) == -EINVAL,
          "stride smaller than width*bpp rejected");
}

static void test_padded_stride_passthrough(drmtap_ctx *ctx) {
    printf("test_padded_stride_passthrough:\n");
    enum { W = 32, H = 8, PAD_PX = 16 };
    const uint32_t src_stride = (W + PAD_PX) * 4; /* padded scanout row */
    static uint32_t src[(W + PAD_PX) * H];
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W + PAD_PX; x++) {
            /* Distinct value per pixel; padding columns get a sentinel that
             * must NOT survive into the tight output. */
            src[y * (W + PAD_PX) + x] =
                (x < W) ? (0xFF000000u | (y << 8) | x) : 0xDEADBEEFu;
        }
    }
    int fd = make_pixel_memfd(src, sizeof(src));
    if (fd < 0) {
        printf("  SKIP: memfd_create failed\n");
        return;
    }

    drmtap_dmabuf_desc desc;
    drmtap_frame_info frame;
    memset(&desc, 0, sizeof(desc));
    desc.dma_buf_fd = fd;
    desc.width = W;
    desc.height = H;
    desc.format = FMT_XR24;
    desc.modifier = 0;
    desc.fb_id = 0;
    desc.num_planes = 1;
    desc.pitches[0] = src_stride;

    int ret = drmtap_convert_dmabuf(ctx, &desc, &frame);
    check(ret == 0, "padded-stride convert succeeds");
    if (ret == 0) {
        check(frame.stride == W * 4,
              "output stride normalized to width*4 (padding dropped)");
        /* Every output row must equal the first W pixels of the source row,
         * with no padding sentinel bleeding in. */
        int content_ok = 1;
        const uint32_t *out = frame.data;
        for (uint32_t y = 0; y < H && content_ok; y++) {
            for (uint32_t x = 0; x < W; x++) {
                if (out[y * W + x] != src[y * (W + PAD_PX) + x]) {
                    content_ok = 0;
                    break;
                }
            }
        }
        check(content_ok, "repacked pixels match source (no padding bleed)");
    }
    close(fd);
}

static void test_grab_guard(drmtap_ctx *ctx, int render_only) {
    printf("test_grab_guard:\n");
    if (!render_only) {
        printf("  SKIP: context is not render-only\n");
        return;
    }
    drmtap_frame_info frame;
    check(drmtap_grab(ctx, &frame) == -ENOTSUP,
          "grab rejected on render-only context");
    check(drmtap_grab_mapped_fast(ctx, &frame) == -ENOTSUP,
          "fast grab rejected on render-only context");
}

static void test_linear_xrgb8888(drmtap_ctx *ctx) {
    printf("test_linear_xrgb8888:\n");
    enum { W = 64, H = 32 };
    const uint32_t stride = W * 4;
    static uint32_t src[W * H];
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            src[y * W + x] = 0xAA000000u | (y << 16) | x;
        }
    }
    int fd = make_pixel_memfd(src, sizeof(src));
    if (fd < 0) {
        printf("  SKIP: memfd_create failed\n");
        return;
    }

    drmtap_dmabuf_desc desc;
    drmtap_frame_info frame;
    memset(&desc, 0, sizeof(desc));
    desc.dma_buf_fd = fd;
    desc.width = W;
    desc.height = H;
    desc.format = FMT_XR24;
    desc.modifier = 0; /* linear */
    desc.fb_id = 0;    /* no caching for a stand-in fd */
    desc.num_planes = 1;
    desc.pitches[0] = stride;

    int ret = drmtap_convert_dmabuf(ctx, &desc, &frame);
    check(ret == 0, "linear XRGB8888 convert succeeds");
    if (ret == 0) {
        check(frame.data != NULL, "pixels returned");
        check(frame.format == FMT_XR24, "format stays XRGB8888");
        check(frame.stride == stride, "stride preserved");
        check(frame.width == W && frame.height == H, "geometry preserved");
        check(frame.data && memcmp(frame.data, src, sizeof(src)) == 0,
              "pixel content identical");

        /* Second convert on the same context must reuse the grow-once buffer
         * and still produce correct pixels. */
        ret = drmtap_convert_dmabuf(ctx, &desc, &frame);
        check(ret == 0 && frame.data &&
              memcmp(frame.data, src, sizeof(src)) == 0,
              "second convert (buffer reuse) identical");
    }
    close(fd);
}

static void test_linear_xr30(drmtap_ctx *ctx) {
    printf("test_linear_xr30:\n");
    enum { W = 16, H = 8 };
    const uint32_t stride = W * 4;
    /* XRGB2101010: X[31:30] R[29:20] G[19:10] B[9:0]. Full-scale R and B,
     * zero G — exact under both truncating and rounding 10->8 reductions. */
    const uint32_t px = (1023u << 20) | (0u << 10) | 1023u;
    static uint32_t src[W * H];
    for (int i = 0; i < W * H; i++) {
        src[i] = px;
    }
    int fd = make_pixel_memfd(src, sizeof(src));
    if (fd < 0) {
        printf("  SKIP: memfd_create failed\n");
        return;
    }

    drmtap_dmabuf_desc desc;
    drmtap_frame_info frame;
    memset(&desc, 0, sizeof(desc));
    desc.dma_buf_fd = fd;
    desc.width = W;
    desc.height = H;
    desc.format = FMT_XR30;
    desc.modifier = 0;
    desc.fb_id = 0;
    desc.num_planes = 1;
    desc.pitches[0] = stride;

    int ret = drmtap_convert_dmabuf(ctx, &desc, &frame);
    check(ret == 0, "linear XR30 convert succeeds");
    if (ret == 0) {
        check(frame.format == FMT_XR24, "XR30 reduced to XRGB8888");
        uint32_t out = frame.data ? ((const uint32_t *)frame.data)[0] : 0;
        check((out & 0x00FFFFFFu) == 0x00FF00FFu,
              "10-bit magenta reduces to 8-bit magenta");
    }
    close(fd);
}

static void test_hdr_pq_xr30(drmtap_ctx *ctx) {
    printf("test_hdr_pq_xr30:\n");
    enum { W = 16, H = 8 };
    const uint32_t stride = W * 4;
    const uint32_t px = (512u << 20) | (512u << 10) | 512u;
    static uint32_t src[W * H];
    for (int i = 0; i < W * H; i++) {
        src[i] = px;
    }
    int fd = make_pixel_memfd(src, sizeof(src));
    if (fd < 0) {
        printf("  SKIP: memfd_create failed\n");
        return;
    }

    drmtap_dmabuf_desc desc;
    drmtap_frame_info frame;
    memset(&desc, 0, sizeof(desc));
    desc.dma_buf_fd = fd;
    desc.width = W;
    desc.height = H;
    desc.format = FMT_XR30;
    desc.modifier = 0;
    desc.fb_id = 0;
    desc.num_planes = 1;
    desc.pitches[0] = stride;
    desc.hdr_eotf = DRMTAP_EOTF_PQ;
    desc.hdr_max_nits = 1000;

    /* Baseline: the SAME 10-bit pixel reduced WITHOUT HDR (plain 10->8). Keep
     * a copy — the next convert overwrites the ctx-owned output buffer. */
    desc.hdr_eotf = DRMTAP_EOTF_SDR;
    desc.hdr_max_nits = 0;
    int r_sdr = drmtap_convert_dmabuf(ctx, &desc, &frame);
    uint32_t sdr_px = (r_sdr == 0 && frame.data)
                          ? (((const uint32_t *)frame.data)[0] & 0x00FFFFFFu) : 0;

    /* HDR10 (PQ) tone-map of the same pixel. */
    desc.hdr_eotf = DRMTAP_EOTF_PQ;
    desc.hdr_max_nits = 1000;
    int ret = drmtap_convert_dmabuf(ctx, &desc, &frame);
    check(ret == 0, "HDR10 (PQ) XR30 convert succeeds");
    if (ret == 0) {
        check(frame.format == FMT_XR24, "tone-mapped output is XRGB8888");
        check(frame.data != NULL, "tone-mapped pixels returned");
        uint32_t pq_px = frame.data
                             ? (((const uint32_t *)frame.data)[0] & 0x00FFFFFFu) : 0;
        /* The whole point of the PQ path: it must NOT collapse to the plain
         * bit-depth reduction. If HDR metadata were ignored, pq_px == sdr_px
         * and this fails — which is exactly the broken-HDR regression to catch. */
        check(r_sdr == 0 && pq_px != sdr_px,
              "PQ tone-map differs from the plain SDR reduction (HDR path engaged)");
    }
    close(fd);
}

int main(void) {
    printf("test_convert: split-capture convert API\n");

    test_null_args();

    /* Prefer a render-only context (the split-model consumer); fall back to a
     * full context — convert works on any ctx with a usable backend. */
    int render_only = 1;
    drmtap_ctx *ctx = drmtap_open_render(NULL);
    if (!ctx) {
        render_only = 0;
        ctx = drmtap_open(NULL);
    }
    if (!ctx) {
        printf("  SKIP: no render node or DRM device available\n");
        printf("test_convert: %d failures\n", failures);
        return failures ? 1 : 0;
    }
    printf("  using %s context\n", render_only ? "render-only" : "full");

    test_desc_validation(ctx);
    test_grab_guard(ctx, render_only);
    test_linear_xrgb8888(ctx);
    test_padded_stride_passthrough(ctx);
    test_linear_xr30(ctx);
    test_hdr_pq_xr30(ctx);

    drmtap_close(ctx);

    printf("test_convert: %d failures\n", failures);
    return failures ? 1 : 0;
}
