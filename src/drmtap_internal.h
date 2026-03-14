/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file drmtap_internal.h
 * @brief Internal header — shared between library modules, NOT public
 *
 * This header exposes the drmtap_ctx struct and internal helper functions
 * so that library modules (enumerate, grab, helper, etc.) can access
 * context fields directly. It is NOT installed with the public header.
 */

#ifndef DRMTAP_INTERNAL_H
#define DRMTAP_INTERNAL_H

#include <stdarg.h>
#include <pthread.h>
#include "drmtap.h"

/* ========================================================================= */
/* Context structure (shared across modules)                                 */
/* ========================================================================= */

struct drmtap_ctx {
    /* DRM device */
    int drm_fd;
    char device_path[256];
    char driver_name[64];

    /* Selected display */
    uint32_t crtc_id;

    /* Cached resources for hotplug detection */
    uint32_t cached_connector_count;
    uint32_t cached_crtc_count;

    /* Helper binary */
    char helper_path[512];
    int helper_pid;
    int helper_fd;          /* socket to helper */

    /* Error handling */
    char error_msg[512];

    /* Thread safety */
    pthread_mutex_t lock;

    /* Debug */
    int debug;
};

/* ========================================================================= */
/* Internal API (used across modules)                                        */
/* ========================================================================= */

// Set error message on context (or global static if ctx is NULL)
void drmtap_set_error(drmtap_ctx *ctx, const char *fmt, ...);

// Debug log to stderr (only when ctx->debug is set)
void drmtap_debug_log(drmtap_ctx *ctx, const char *fmt, ...);

#endif /* DRMTAP_INTERNAL_H */
