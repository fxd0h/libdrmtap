/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file drm_enumerate.c
 * @brief Plane, CRTC, and connector enumeration via DRM/KMS
 */

#include <string.h>
#include <errno.h>

#include "drmtap.h"

int drmtap_list_displays(drmtap_ctx *ctx, drmtap_display *out, int max_count) {
    if (!ctx || !out || max_count <= 0) {
        return -EINVAL;
    }

    /* TODO: Phase 1 — enumerate via drmModeGetResources, drmModeGetConnector */
    (void)out;
    (void)max_count;
    return 0;
}

int drmtap_displays_changed(drmtap_ctx *ctx) {
    if (!ctx) {
        return -EINVAL;
    }

    /* TODO: Phase 1 — compare current resources with cached state */
    return 0;
}
