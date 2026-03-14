/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file gpu_amd.c
 * @brief AMD GPU backend — CPU deswizzle with VAAPI/SDMA upgrade path
 *
 * AMD GPUs use DCC (Delta Color Compression) and various tiling modes.
 * This backend uses CPU deswizzle for now, with VAAPI and SDMA copy
 * as future upgrade paths for hardware-accelerated format conversion.
 *
 * AMD uses vendor-specific modifiers with vendor = 0x02 (AMD).
 * Common formats: XRGB8888, ABGR8888, ABGR16161616 (HDR).
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>

#include "drmtap_internal.h"
#include "drmtap.h"

/* AMD modifier vendor byte */
#define AMD_VENDOR 0x02

/* ========================================================================= */
/* Backend API                                                               */
/* ========================================================================= */

int drmtap_gpu_amd_match(const char *driver) {
    if (!driver) {
        return 0;
    }
    return (strcmp(driver, "amdgpu") == 0 || strcmp(driver, "radeon") == 0);
}

int drmtap_gpu_amd_process(drmtap_ctx *ctx, void *data,
                           uint32_t width, uint32_t height,
                           uint32_t stride, uint32_t format,
                           uint64_t modifier) {
    (void)format;

    /* Linear — no conversion needed */
    if (modifier == 0) {
        drmtap_debug_log(ctx, "amd: linear framebuffer, no conversion");
        return 0;
    }

    /* Check if it's an AMD modifier */
    uint8_t vendor = (uint8_t)(modifier >> 56);
    if (vendor == AMD_VENDOR) {
        drmtap_debug_log(ctx, "amd: tiled modifier 0x%lx, CPU deswizzle",
                         (unsigned long)modifier);

        /* Use generic deswizzle — AMD tiling falls back to linear copy
         * for unknown modifiers, which is safe for many AMD configs
         * where the compositor outputs linear despite the modifier. */
        size_t size = (size_t)stride * height;
        void *tmp = malloc(size);
        if (!tmp) {
            return -ENOMEM;
        }

        int ret = drmtap_deswizzle(data, tmp, width, height,
                                    stride, stride, modifier);
        if (ret == 0) {
            memcpy(data, tmp, size);
        }
        free(tmp);

        /* TODO: VAAPI hardware path:
         *   1. Import DMA-BUF into VADisplay (radeonsi)
         *   2. vaPutImage() to convert tiled→linear
         * TODO: SDMA copy path:
         *   1. Use AMDGPU SDMA engine to copy FB to a linear BO
         *   2. mmap the linear BO
         *   This bypasses CPU entirely — ideal for high-res/HDR. */

        return ret;
    }

    /* Non-AMD modifier on AMD GPU (unusual but possible) */
    drmtap_debug_log(ctx, "amd: non-AMD modifier 0x%lx, passing through",
                     (unsigned long)modifier);
    return 0;
}
