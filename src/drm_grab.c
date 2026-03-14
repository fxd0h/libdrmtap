/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file drm_grab.c
 * @brief Framebuffer capture via GetFB2, Prime handle export, and mmap
 */

#include <errno.h>

#include "drmtap.h"

int drmtap_grab(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    if (!ctx || !frame) {
        return -EINVAL;
    }

    /* TODO: Phase 2 — drmModeGetFB2, drmPrimeHandleToFD */
    return -ENOSYS;
}

int drmtap_grab_mapped(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    if (!ctx || !frame) {
        return -EINVAL;
    }

    /* TODO: Phase 2 — grab + mmap + deswizzle if needed */
    return -ENOSYS;
}

void drmtap_frame_release(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    (void)ctx;
    if (!frame) {
        return;
    }

    /* TODO: Phase 2 — munmap, close fd */
    frame->data = NULL;
    frame->dma_buf_fd = -1;
}
