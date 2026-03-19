/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file gpu_nvidia.c
 * @brief Nvidia GPU backend — dumb buffer export + CPU deswizzle
 *
 * Nvidia GPUs with the open-source nvidia-drm kernel module use
 * blocklinear tiling (16×128 pixel tiles). Since VAAPI is not natively
 * supported on Nvidia, we use CPU deswizzle.
 *
 * The capture path for Nvidia differs from Intel/AMD:
 *   1. drmModeGetFB2() to get handles
 *   2. DRM_IOCTL_GEM_FLINK + DRM_IOCTL_GEM_OPEN to get a dumb handle
 *   3. DRM_IOCTL_MODE_MAP_DUMB to get mmap offset
 *   4. mmap the DRM fd (not a DMA-BUF fd)
 *   5. CPU deswizzle from blocklinear to linear
 *
 * Status: experimental (following kmsvnc's approach)
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>

#include "drmtap_internal.h"
#include "drmtap.h"

/* Nvidia modifier vendor byte */
#define NV_VENDOR 0x10

/* ========================================================================= */
/* Backend API                                                               */
/* ========================================================================= */

int drmtap_gpu_nvidia_match(const char *driver) {
    if (!driver) {
        return 0;
    }
    return (strcmp(driver, "nvidia-drm") == 0 ||
            strcmp(driver, "nvidia") == 0 ||
            strcmp(driver, "nouveau") == 0);
}

int drmtap_gpu_nvidia_process(drmtap_ctx *ctx, void *data,
                              uint32_t width, uint32_t height,
                              uint32_t stride, uint32_t format,
                              uint64_t modifier) {
    (void)format;

    /* Linear — no conversion needed (rare on Nvidia but possible) */
    if (modifier == 0) {
        drmtap_debug_log(ctx, "nvidia: linear framebuffer, no conversion");
        return 0;
    }

    /* Check if it's an Nvidia modifier */
    uint8_t vendor = (uint8_t)(modifier >> 56);
    if (vendor == NV_VENDOR) {
        drmtap_debug_log(ctx,
                         "nvidia: blocklinear modifier 0x%lx, CPU deswizzle",
                         (unsigned long)modifier);

        /* Deswizzle using our Nvidia X-TILED (16×128) algorithm */
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
        return ret;
    }

    /* nouveau may use different modifiers */
    drmtap_debug_log(ctx, "nvidia: unknown modifier 0x%lx, passing through",
                     (unsigned long)modifier);
    return 0;
}
