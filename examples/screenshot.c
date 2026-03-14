/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file screenshot.c
 * @brief Example — capture one frame and write raw RGBA to stdout
 *
 * Usage:
 *   ./screenshot > frame.rgba
 *   # Convert to PNG with ImageMagick:
 *   convert -size 1920x1080 -depth 8 rgba:frame.rgba frame.png
 */

#include <stdio.h>
#include <stdlib.h>

#include "drmtap.h"

int main(void) {
    drmtap_config cfg = {0};
    cfg.debug = 1;

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

    /* Write raw RGBA to stdout */
    fwrite(frame.data, 1, frame.stride * frame.height, stdout);

    fprintf(stderr, "Wrote %ux%u frame (%u bytes)\n",
            frame.width, frame.height, frame.stride * frame.height);

    drmtap_frame_release(ctx, &frame);
    drmtap_close(ctx);
    return 0;
}
