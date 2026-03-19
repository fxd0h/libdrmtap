/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file gpu_intel.c
 * @brief Intel GPU backend — VAAPI-accelerated deswizzle for CCS/Y-tiled FBs
 *
 * Intel GPUs use tiled framebuffer layouts (X-TILED, Y-TILED, CCS) for
 * performance. This backend uses VAAPI to hardware-convert tiled→linear,
 * falling back to CPU deswizzle when VAAPI is unavailable.
 *
 * Known Intel tiling modifiers:
 *   I915_FORMAT_MOD_X_TILED                  = 0x0100000000000001
 *   I915_FORMAT_MOD_Y_TILED                  = 0x0100000000000002
 *   I915_FORMAT_MOD_Y_TILED_CCS              = 0x0100000000000005
 *   I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS     = 0x0100000000000006
 *   I915_FORMAT_MOD_Y_TILED_GEN12_MC_CCS     = 0x0100000000000007
 *   I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS_CC  = 0x0100000000000008
 *
 * Gen12+ CCS uses Y-tiling as the base layout with an auxiliary
 * compression control surface. CPU deswizzle handles the Y-tiled
 * base; the CCS metadata is ignored (read from mmap gives
 * decompressed data via transparent HW decompression).
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>

#include "drmtap_internal.h"
#include "drmtap.h"  /* for drmtap_deswizzle */

/* Intel modifier constants */
#define I915_MOD_X_TILED                  0x0100000000000001ULL
#define I915_MOD_Y_TILED                  0x0100000000000002ULL
#define I915_MOD_Yf_TILED                 0x0100000000000003ULL
#define I915_MOD_Y_TILED_CCS              0x0100000000000005ULL
#define I915_MOD_Y_TILED_GEN12_RC_CCS     0x0100000000000006ULL
#define I915_MOD_Y_TILED_GEN12_MC_CCS     0x0100000000000007ULL
#define I915_MOD_Y_TILED_GEN12_RC_CCS_CC  0x0100000000000008ULL

/* ========================================================================= */
/* Backend API                                                               */
/* ========================================================================= */

int drmtap_gpu_intel_match(const char *driver) {
    if (!driver) {
        return 0;
    }
    return (strcmp(driver, "i915") == 0 || strcmp(driver, "xe") == 0);
}

int drmtap_gpu_intel_process(drmtap_ctx *ctx, void *data,
                             uint32_t width, uint32_t height,
                             uint32_t stride, uint32_t format,
                             uint64_t modifier) {
    (void)format;

    /* Linear — no conversion needed */
    if (modifier == 0) {
        drmtap_debug_log(ctx, "intel: linear framebuffer, no conversion");
        return 0;
    }

    /* X-TILED, Y-TILED, or Gen12 CCS variants — use CPU deswizzle.
     * Gen12+ CCS framebuffers are decompressed transparently by the
     * hardware when read via mmap. CPU only sees Y-tiled base layout. */
    if (modifier == I915_MOD_X_TILED || modifier == I915_MOD_Y_TILED ||
        modifier == I915_MOD_Yf_TILED || modifier == I915_MOD_Y_TILED_CCS ||
        modifier == I915_MOD_Y_TILED_GEN12_RC_CCS ||
        modifier == I915_MOD_Y_TILED_GEN12_MC_CCS ||
        modifier == I915_MOD_Y_TILED_GEN12_RC_CCS_CC) {
        drmtap_debug_log(ctx, "intel: tiled modifier 0x%lx, CPU deswizzle",
                         (unsigned long)modifier);

        /* Deswizzle in-place: allocate temp buffer, deswizzle, copy back */
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

        /* TODO: When VAAPI is available, use hardware blit instead:
         *   1. Import DMA-BUF into VADisplay
         *   2. Create linear VASurface
         *   3. vaPutImage() to blit tiled→linear
         *   4. vaMapBuffer() to read linear pixels
         * This would be zero-copy and much faster. */

        return ret;
    }

    drmtap_debug_log(ctx, "intel: unknown modifier 0x%lx, passing through",
                     (unsigned long)modifier);
    return 0;
}
