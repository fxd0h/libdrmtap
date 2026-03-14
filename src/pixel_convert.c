/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file pixel_convert.c
 * @brief Pixel format conversion, GPU tiling deswizzle, and frame differencing
 */

#include <errno.h>
#include <string.h>
#include <stdint.h>

#include "drmtap.h"

int drmtap_diff_frames(const void *frame_a, const void *frame_b,
                       uint32_t width, uint32_t height, uint32_t stride,
                       drmtap_rect *rects_out, int max_rects,
                       int tile_size) {
    if (!frame_a || !frame_b || !rects_out || max_rects <= 0 || tile_size <= 0) {
        return -EINVAL;
    }

    int dirty_count = 0;
    uint32_t tiles_x = (width + (uint32_t)tile_size - 1) / (uint32_t)tile_size;
    uint32_t tiles_y = (height + (uint32_t)tile_size - 1) / (uint32_t)tile_size;

    for (uint32_t ty = 0; ty < tiles_y; ty++) {
        for (uint32_t tx = 0; tx < tiles_x; tx++) {
            uint32_t rx = tx * (uint32_t)tile_size;
            uint32_t ry = ty * (uint32_t)tile_size;
            uint32_t rw = (rx + (uint32_t)tile_size > width)
                          ? width - rx : (uint32_t)tile_size;
            uint32_t rh = (ry + (uint32_t)tile_size > height)
                          ? height - ry : (uint32_t)tile_size;

            int dirty = 0;
            for (uint32_t row = ry; row < ry + rh && !dirty; row++) {
                const uint8_t *a = (const uint8_t *)frame_a + row * stride + rx * 4;
                const uint8_t *b = (const uint8_t *)frame_b + row * stride + rx * 4;
                if (memcmp(a, b, rw * 4) != 0) {
                    dirty = 1;
                }
            }

            if (dirty) {
                if (dirty_count < max_rects) {
                    rects_out[dirty_count].x = rx;
                    rects_out[dirty_count].y = ry;
                    rects_out[dirty_count].w = rw;
                    rects_out[dirty_count].h = rh;
                }
                dirty_count++;
            }
        }
    }

    return dirty_count;
}
