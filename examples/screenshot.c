/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file screenshot.c
 * @brief Example — capture one frame and write PPM to stdout
 *
 * Usage:
 *   ./screenshot > screenshot.ppm
 *   ./screenshot /dev/dri/card1 > screenshot.ppm
 *
 * Output is PPM P6 (binary RGB), viewable with any image viewer.
 * Handles XRGB8888 and ARGB8888 pixel formats (most common).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "drmtap.h"

int main(int argc, char *argv[]) {
    drmtap_config cfg = {0};
    cfg.debug = 1;

    /* Optional: specify device path */
    if (argc > 1) {
        cfg.device_path = argv[1];
    }

    drmtap_ctx *ctx = drmtap_open(&cfg);
    if (!ctx) {
        fprintf(stderr, "Failed to open: %s\n", drmtap_error(NULL));
        return 1;
    }

    /* List displays */
    drmtap_display displays[8];
    int n = drmtap_list_displays(ctx, displays, 8);
    if (n <= 0) {
        fprintf(stderr, "No displays found\n");
        drmtap_close(ctx);
        return 1;
    }

    fprintf(stderr, "Capturing %s (%ux%u)...\n",
            displays[0].name, displays[0].width, displays[0].height);

    /* Capture */
    drmtap_frame_info frame = {0};
    int ret = drmtap_grab_mapped(ctx, &frame);
    if (ret < 0) {
        fprintf(stderr, "Capture failed: %s\n", drmtap_error(ctx));
        drmtap_close(ctx);
        return 1;
    }

    if (!frame.data) {
        fprintf(stderr, "No pixel data (mmap failed)\n");
        drmtap_frame_release(ctx, &frame);
        drmtap_close(ctx);
        return 1;
    }

    /* Write PPM P6 header */
    fprintf(stdout, "P6\n%u %u\n255\n", frame.width, frame.height);

    /* Convert XRGB8888/ARGB8888 → RGB and write row by row
     *
     * XRGB8888 layout in memory (little-endian):
     *   byte[0] = B, byte[1] = G, byte[2] = R, byte[3] = X/A
     *
     * PPM P6 needs: R, G, B per pixel
     */
    uint8_t *rgb_row = malloc(frame.width * 3);
    if (!rgb_row) {
        fprintf(stderr, "malloc failed\n");
        drmtap_frame_release(ctx, &frame);
        drmtap_close(ctx);
        return 1;
    }

    const uint8_t *src = (const uint8_t *)frame.data;
    for (uint32_t y = 0; y < frame.height; y++) {
        const uint8_t *row = src + y * frame.stride;
        for (uint32_t x = 0; x < frame.width; x++) {
            /* XRGB8888 little-endian: B G R X */
            rgb_row[x * 3 + 0] = row[x * 4 + 2];  /* R */
            rgb_row[x * 3 + 1] = row[x * 4 + 1];  /* G */
            rgb_row[x * 3 + 2] = row[x * 4 + 0];  /* B */
        }
        fwrite(rgb_row, 1, frame.width * 3, stdout);
    }

    free(rgb_row);

    fprintf(stderr, "Wrote %ux%u PPM (%u bytes pixel data)\n",
            frame.width, frame.height, frame.stride * frame.height);

    drmtap_frame_release(ctx, &frame);
    drmtap_close(ctx);
    return 0;
}
