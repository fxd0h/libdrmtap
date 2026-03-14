/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file drmtap.c
 * @brief Context management, error handling, version, and debug logging
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <pthread.h>

#include "drmtap.h"

/* ========================================================================= */
/* Internal context structure                                                */
/* ========================================================================= */

struct drmtap_ctx {
    /* DRM device */
    int drm_fd;
    char device_path[256];
    char driver_name[64];

    /* Selected display */
    uint32_t crtc_id;

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

/* Static error for when ctx is NULL (from failed drmtap_open) */
static char g_static_error[512] = "";

/* ========================================================================= */
/* Internal helpers                                                          */
/* ========================================================================= */

// Set human-readable error message on context (or global if ctx is NULL)
static void set_error(drmtap_ctx *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (ctx) {
        vsnprintf(ctx->error_msg, sizeof(ctx->error_msg), fmt, ap);
    } else {
        vsnprintf(g_static_error, sizeof(g_static_error), fmt, ap);
    }
    va_end(ap);
}

// Log debug message to stderr (only when debug is enabled)
static void debug_log(drmtap_ctx *ctx, const char *fmt, ...) {
    if (!ctx || !ctx->debug) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[drmtap] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ========================================================================= */
/* Public API                                                                */
/* ========================================================================= */

int drmtap_version(void) {
    return (DRMTAP_VERSION_MAJOR << 16) |
           (DRMTAP_VERSION_MINOR << 8) |
           DRMTAP_VERSION_PATCH;
}

drmtap_ctx *drmtap_open(const drmtap_config *config) {
    drmtap_ctx *ctx = calloc(1, sizeof(drmtap_ctx));
    if (!ctx) {
        set_error(NULL, "Failed to allocate context: %s", strerror(errno));
        return NULL;
    }

    ctx->drm_fd = -1;
    ctx->helper_pid = -1;
    ctx->helper_fd = -1;

    if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
        set_error(NULL, "Failed to init mutex: %s", strerror(errno));
        free(ctx);
        return NULL;
    }

    /* Parse config */
    if (config) {
        ctx->debug = config->debug;
        ctx->crtc_id = config->crtc_id;

        if (config->device_path) {
            snprintf(ctx->device_path, sizeof(ctx->device_path),
                     "%s", config->device_path);
        }
        if (config->helper_path) {
            snprintf(ctx->helper_path, sizeof(ctx->helper_path),
                     "%s", config->helper_path);
        }
    }

    /* Check DRMTAP_DEBUG env var */
    const char *dbg_env = getenv("DRMTAP_DEBUG");
    if (dbg_env && dbg_env[0] == '1') {
        ctx->debug = 1;
    }

    debug_log(ctx, "drmtap v%d.%d.%d opening",
              DRMTAP_VERSION_MAJOR, DRMTAP_VERSION_MINOR, DRMTAP_VERSION_PATCH);

    /* TODO: Phase 1 — open DRM device, detect GPU driver */
    /* TODO: Phase 3 — locate and spawn helper if needed */

    debug_log(ctx, "context opened successfully");
    return ctx;
}

void drmtap_close(drmtap_ctx *ctx) {
    if (!ctx) {
        return;
    }

    debug_log(ctx, "closing context");

    /* TODO: Phase 3 — kill helper process */
    /* TODO: Phase 1 — close DRM fd */

    if (ctx->drm_fd >= 0) {
        /* close(ctx->drm_fd); */
        ctx->drm_fd = -1;
    }

    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
}

const char *drmtap_error(drmtap_ctx *ctx) {
    if (ctx) {
        return ctx->error_msg[0] ? ctx->error_msg : NULL;
    }
    return g_static_error[0] ? g_static_error : NULL;
}

const char *drmtap_gpu_driver(drmtap_ctx *ctx) {
    if (!ctx || !ctx->driver_name[0]) {
        return NULL;
    }
    return ctx->driver_name;
}
