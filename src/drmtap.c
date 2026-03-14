/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file drmtap.c
 * @brief Context management, DRM device open, error handling, and debug logging
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drmtap_internal.h"

/* Static error for when ctx is NULL (from failed drmtap_open) */
static char g_static_error[512] = "";

/* ========================================================================= */
/* Internal helpers (exported via drmtap_internal.h)                         */
/* ========================================================================= */

void drmtap_set_error(drmtap_ctx *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (ctx) {
        vsnprintf(ctx->error_msg, sizeof(ctx->error_msg), fmt, ap);
    } else {
        vsnprintf(g_static_error, sizeof(g_static_error), fmt, ap);
    }
    va_end(ap);
}

void drmtap_debug_log(drmtap_ctx *ctx, const char *fmt, ...) {
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
/* DRM device discovery                                                      */
/* ========================================================================= */

// Auto-detect DRM device: scan /dev/dri/card* for one with KMS resources
static int open_drm_auto(drmtap_ctx *ctx) {
    char path[64];

    /* Check DRM_DEVICE env var first */
    const char *env_dev = getenv("DRM_DEVICE");
    if (env_dev) {
        drmtap_debug_log(ctx, "trying DRM_DEVICE=%s", env_dev);
        int fd = open(env_dev, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            snprintf(ctx->device_path, sizeof(ctx->device_path), "%s", env_dev);
            return fd;
        }
        drmtap_debug_log(ctx, "DRM_DEVICE open failed: %s", strerror(errno));
    }

    /* Scan /dev/dri/card0 through card15 */
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        drmtap_debug_log(ctx, "probing %s", path);

        /* Check if this device has DRM resources (is a KMS device) */
        drmModeRes *res = drmModeGetResources(fd);
        if (!res) {
            drmtap_debug_log(ctx, "  no KMS resources, skipping");
            close(fd);
            continue;
        }
        drmModeFreeResources(res);

        snprintf(ctx->device_path, sizeof(ctx->device_path), "%s", path);
        return fd;
    }

    return -1;
}

// Detect GPU driver name from the DRM fd
static void detect_driver(drmtap_ctx *ctx) {
    drmVersion *ver = drmGetVersion(ctx->drm_fd);
    if (ver) {
        snprintf(ctx->driver_name, sizeof(ctx->driver_name), "%.*s",
                 ver->name_len, ver->name);
        drmtap_debug_log(ctx, "GPU driver: %s (v%d.%d.%d)",
                         ctx->driver_name, ver->version_major,
                         ver->version_minor, ver->version_patchlevel);
        drmFreeVersion(ver);
    } else {
        drmtap_debug_log(ctx, "warning: could not get DRM version");
    }
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
        drmtap_set_error(NULL, "Failed to allocate context: %s",
                         strerror(errno));
        return NULL;
    }

    ctx->drm_fd = -1;
    ctx->helper_pid = -1;
    ctx->helper_fd = -1;

    if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
        drmtap_set_error(NULL, "Failed to init mutex: %s", strerror(errno));
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

    drmtap_debug_log(ctx, "drmtap v%d.%d.%d opening",
                     DRMTAP_VERSION_MAJOR, DRMTAP_VERSION_MINOR,
                     DRMTAP_VERSION_PATCH);

    /* Open DRM device */
    if (ctx->device_path[0]) {
        /* Explicit device path */
        ctx->drm_fd = open(ctx->device_path, O_RDWR | O_CLOEXEC);
        if (ctx->drm_fd < 0) {
            drmtap_set_error(NULL, "Failed to open %s: %s",
                             ctx->device_path, strerror(errno));
            goto fail;
        }
    } else {
        /* Auto-detect */
        ctx->drm_fd = open_drm_auto(ctx);
        if (ctx->drm_fd < 0) {
            drmtap_set_error(NULL, "No DRM device found — is a GPU attached?");
            goto fail;
        }
    }

    /* Detect GPU driver */
    detect_driver(ctx);

    /* Set universal planes — needed for cursor plane detection */
    if (drmSetClientCap(ctx->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0) {
        drmtap_debug_log(ctx,
                         "warning: DRM_CLIENT_CAP_UNIVERSAL_PLANES not supported");
    }

    drmtap_debug_log(ctx, "context opened: %s (%s)",
                     ctx->device_path, ctx->driver_name);

    /* TODO: Phase 3 — locate and spawn helper if needed */

    return ctx;

fail:
    if (ctx->drm_fd >= 0) {
        close(ctx->drm_fd);
    }
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
    return NULL;
}

void drmtap_close(drmtap_ctx *ctx) {
    if (!ctx) {
        return;
    }

    drmtap_debug_log(ctx, "closing context");

    /* TODO: Phase 3 — kill helper process */

    if (ctx->drm_fd >= 0) {
        close(ctx->drm_fd);
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
