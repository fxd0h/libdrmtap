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

#include <drm_fourcc.h>

#include "drmtap_internal.h"
#include "drmtap.h"

/* The vendor byte comes from libdrm, never a literal: this was 0x10, which is the low
 * byte of the block-linear encoding rather than a vendor, so the backend never matched
 * a real Nvidia modifier. See the note in pixel_convert.c.
 * https://gitlab.freedesktop.org/mesa/drm/-/blob/main/include/drm/drm_fourcc.h */
#define DRMTAP_NV_VENDOR DRM_FORMAT_MOD_VENDOR_NVIDIA

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
    /* Only `modifier` decides anything on this backend now: linear needs no work and
     * block-linear has no CPU decoder, so the pixel arguments are never read. */
    (void)data;
    (void)width;
    (void)height;
    (void)stride;

    /* Linear — no conversion needed (rare on Nvidia but possible) */
    if (modifier == 0) {
        drmtap_debug_log(ctx, "nvidia: linear framebuffer, no conversion");
        return 0;
    }

    /* Check if it's an Nvidia modifier */
    uint8_t vendor = (uint8_t)(modifier >> 56);
    if (vendor == DRMTAP_NV_VENDOR) {
        /* There is no CPU decoder for Nvidia block-linear here: the one that used to
         * be called from this branch tested vendor byte 0x10, which is not a vendor at
         * all, so it never ran on a real modifier and was removed rather than trusted.
         * `drmtap_deswizzle` answers -ENOTSUP for this vendor, so going through it
         * would allocate a full frame only to throw it away -- and report -ENOMEM
         * instead of the truth if that allocation failed. Say it directly. */
        drmtap_set_error(ctx,
                         "nvidia: block-linear modifier 0x%lx has no CPU decoder; "
                         "use the EGL path or a linear scanout",
                         (unsigned long)modifier);
        return -ENOTSUP;
    }

    /* nouveau may use different modifiers */
    drmtap_debug_log(ctx, "nvidia: unknown modifier 0x%lx, passing through",
                     (unsigned long)modifier);
    return 0;
}
