/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file cursor.c
 * @brief Cursor plane capture — position, image, and hotspot
 */

#include <errno.h>

#include "drmtap.h"

int drmtap_get_cursor(drmtap_ctx *ctx, drmtap_cursor_info *cursor) {
    if (!ctx || !cursor) {
        return -EINVAL;
    }

    /* TODO: Phase 5 — read cursor plane from DRM */
    return -ENOSYS;
}

void drmtap_cursor_release(drmtap_ctx *ctx, drmtap_cursor_info *cursor) {
    (void)ctx;
    if (!cursor) {
        return;
    }
    cursor->pixels = NULL;
}
