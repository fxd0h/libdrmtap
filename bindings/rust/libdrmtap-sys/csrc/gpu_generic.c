/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file gpu_generic.c
 * @brief Generic/VM backend for linear framebuffers (virtio, vmwgfx, vbox, vkms)
 *
 * This is the simplest backend — used when the framebuffer modifier is
 * DRM_FORMAT_MOD_LINEAR (0). No deswizzle needed, just direct mmap read.
 * Works with: virtio_gpu, vmwgfx, vboxvideo, vkms, and any driver that
 * outputs linear framebuffers.
 */

#include <string.h>
#include <errno.h>
#include <stdint.h>

#include "drmtap_internal.h"

/* ========================================================================= */
/* Generic linear capture                                                    */
/* ========================================================================= */

/**
 * Check if this backend should handle the given driver.
 * Returns 1 if the driver produces linear framebuffers.
 */
int drmtap_gpu_generic_match(const char *driver) {
    if (!driver) {
        return 0;
    }
    /* Known linear-only drivers */
    if (strcmp(driver, "vkms") == 0 ||
        strcmp(driver, "vmwgfx") == 0 ||
        strcmp(driver, "vboxvideo") == 0 ||
        strcmp(driver, "virtio_gpu") == 0 ||
        strcmp(driver, "bochs-drm") == 0 ||
        strcmp(driver, "cirrus") == 0 ||
        strcmp(driver, "simpledrm") == 0 ||
        strcmp(driver, "qxl") == 0) {
        return 1;
    }
    return 0;
}

/**
 * Process a captured frame for a generic/linear driver.
 * For linear framebuffers, no conversion is needed — the data from
 * mmap is already in row-major linear order.
 *
 * Returns 0 (success) since no processing is needed for linear data.
 */
int drmtap_gpu_generic_process(drmtap_ctx *ctx, void *data,
                               uint32_t width, uint32_t height,
                               uint32_t stride, uint32_t format,
                               uint64_t modifier) {
    (void)data;
    (void)width;
    (void)height;
    (void)stride;
    (void)format;

    if (modifier != 0) {
        drmtap_debug_log(ctx, "generic backend: unexpected modifier 0x%lx",
                         (unsigned long)modifier);
        return -ENOTSUP;
    }

    drmtap_debug_log(ctx, "generic backend: linear data, no conversion needed");
    return 0;
}
