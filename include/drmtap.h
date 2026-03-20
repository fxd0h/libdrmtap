/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file drmtap.h
 * @brief Public API for libdrmtap — types, functions, and constants
 */

#ifndef DRMTAP_H
#define DRMTAP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Version                                                                   */
/* ========================================================================= */

#define DRMTAP_VERSION_MAJOR 0
#define DRMTAP_VERSION_MINOR 1
#define DRMTAP_VERSION_PATCH 0

/**
 * @brief Get the library version as a packed integer.
 * @return (major << 16) | (minor << 8) | patch
 */
int drmtap_version(void);

/* ========================================================================= */
/* Configuration                                                             */
/* ========================================================================= */

/**
 * @brief Configuration for opening a capture context.
 *
 * All fields are optional — pass NULL to drmtap_open() for all defaults.
 */
typedef struct {
    /** DRM device path (e.g., "/dev/dri/card0").
     *  NULL = auto-detect first device with active displays. */
    const char *device_path;

    /** CRTC id to capture (from drmtap_list_displays).
     *  0 = auto-select primary display. */
    uint32_t crtc_id;

    /** Path to privileged helper binary.
     *  NULL = search default locations:
     *    1. $DRMTAP_HELPER_PATH (env var)
     *    2. <exe_dir>/drmtap-helper
     *    3. /usr/libexec/drmtap-helper
     *    4. /usr/local/libexec/drmtap-helper */
    const char *helper_path;

    /** Enable debug logging to stderr.
     *  Can also be enabled with DRMTAP_DEBUG=1 env var. */
    int debug;
} drmtap_config;

/* ========================================================================= */
/* Context                                                                   */
/* ========================================================================= */

/** Opaque handle to a capture session. */
typedef struct drmtap_ctx drmtap_ctx;

/**
 * @brief Open a capture context.
 *
 * @param config Configuration (NULL for all defaults: auto-detect GPU/display)
 * @return Context handle, or NULL on error (call drmtap_error(NULL) for message)
 */
drmtap_ctx *drmtap_open(const drmtap_config *config);

/**
 * @brief Close the context and free all resources.
 * @param ctx Context to close (safe to pass NULL)
 */
void drmtap_close(drmtap_ctx *ctx);

/* ========================================================================= */
/* Display Enumeration                                                       */
/* ========================================================================= */

/** Information about a connected display. */
typedef struct {
    uint32_t crtc_id;       /**< Use in drmtap_config.crtc_id */
    uint32_t connector_id;  /**< DRM connector id */
    char name[32];          /**< e.g., "HDMI-A-1", "DP-2", "eDP-1" */
    uint32_t width;         /**< Current mode width in pixels */
    uint32_t height;        /**< Current mode height in pixels */
    uint32_t refresh_hz;    /**< Vertical refresh rate */
    int active;             /**< 1 = display is on, 0 = disabled */
} drmtap_display;

/**
 * @brief List connected displays.
 *
 * @param ctx       Capture context
 * @param out       Array to fill with display info
 * @param max_count Maximum entries to write
 * @return Number of displays found (may be > max_count), or negative errno
 */
int drmtap_list_displays(drmtap_ctx *ctx, drmtap_display *out, int max_count);

/**
 * @brief Check if display configuration changed since last call.
 *
 * Useful for detecting monitor hotplug events.
 *
 * @param ctx Capture context
 * @return 1 = changed, 0 = unchanged, negative errno on error
 */
int drmtap_displays_changed(drmtap_ctx *ctx);

/* ========================================================================= */
/* Frame Capture                                                             */
/* ========================================================================= */

/** Captured frame metadata and pixel data. */
typedef struct {
    void *data;             /**< Pixel data (mapped path) or NULL (zero-copy) */
    int dma_buf_fd;         /**< DMA-BUF fd (zero-copy) or -1 (mapped) */
    uint32_t width;         /**< Frame width in pixels */
    uint32_t height;        /**< Frame height in pixels */
    uint32_t stride;        /**< Bytes per row (may include padding) */
    uint32_t format;        /**< DRM fourcc (e.g., DRM_FORMAT_XRGB8888) */
    uint64_t modifier;      /**< Format modifier (e.g., DRM_FORMAT_MOD_LINEAR) */
    uint32_t fb_id;         /**< KMS framebuffer id — changes on compositor page flip */
    void *_priv;            /**< Internal — do not touch */
} drmtap_frame_info;

/**
 * @brief Capture a frame — zero-copy path.
 *
 * Returns a DMA-BUF file descriptor in frame->dma_buf_fd that can be
 * passed directly to VAAPI/V4L2 encoders without copying pixel data.
 *
 * @param ctx   Capture context
 * @param frame Output frame info (caller-allocated)
 * @return 0 on success, negative errno on error
 * @retval -EACCES No permission (helper not found or not configured)
 * @retval -ENODEV Display disconnected or CRTC inactive
 * @retval -EBUSY  Previous frame not released
 */
int drmtap_grab(drmtap_ctx *ctx, drmtap_frame_info *frame);

/**
 * @brief Capture a frame — mapped path.
 *
 * Returns a pointer to linear RGBA pixel data in frame->data.
 * Handles GPU tiling → linear conversion automatically.
 *
 * @param ctx   Capture context
 * @param frame Output frame info (caller-allocated)
 * @return 0 on success, negative errno on error
 */
int drmtap_grab_mapped(drmtap_ctx *ctx, drmtap_frame_info *frame);

/**
 * @brief Capture a frame — fast persistent-mmap path.
 *
 * Like drmtap_grab_mapped() but keeps the mmap/GEM handle/prime fd alive
 * between calls. Uses fb_id to detect compositor page flips — if the
 * framebuffer hasn't changed, returns 1 immediately (~0.1ms instead of ~18ms).
 *
 * The returned frame->data pointer is valid until the NEXT call to this
 * function OR until drmtap_close(). Do NOT call drmtap_frame_release()
 * on frames from this function — cleanup is automatic.
 *
 * @param ctx   Capture context
 * @param frame Output frame info (caller-allocated, reused between calls)
 * @return 0 = new frame captured, 1 = same as last call (unchanged),
 *         negative errno on error
 */
int drmtap_grab_mapped_fast(drmtap_ctx *ctx, drmtap_frame_info *frame);

/**
 * @brief Release a captured frame's resources.
 *
 * Must be called after drmtap_grab() or drmtap_grab_mapped().
 * Safe to call with NULL ctx or zeroed frame.
 *
 * @param ctx   Capture context
 * @param frame Frame to release
 */
void drmtap_frame_release(drmtap_ctx *ctx, drmtap_frame_info *frame);

/* ========================================================================= */
/* Cursor Capture                                                            */
/* ========================================================================= */

/** Cursor state: position, image, and visibility. */
typedef struct {
    int32_t x, y;           /**< Cursor position on screen (pixels) */
    int32_t hot_x, hot_y;   /**< Hotspot offset within cursor image */
    uint32_t width, height; /**< Cursor image dimensions */
    uint32_t *pixels;       /**< ARGB8888 premultiplied alpha (NULL if hidden) */
    int visible;            /**< 1 = visible, 0 = hidden */
    void *_priv;            /**< Internal — do not touch */
} drmtap_cursor_info;

/**
 * @brief Get current cursor state (position + image).
 *
 * Cursor is returned separately from the framebuffer so remote desktop
 * clients can render it on the client side for lower latency.
 *
 * @param ctx    Capture context
 * @param cursor Output cursor info (caller-allocated)
 * @return 0 on success, -ENOENT if no cursor plane, negative errno on error
 */
int drmtap_get_cursor(drmtap_ctx *ctx, drmtap_cursor_info *cursor);

/**
 * @brief Release cursor resources.
 * @param ctx    Capture context
 * @param cursor Cursor to release
 */
void drmtap_cursor_release(drmtap_ctx *ctx, drmtap_cursor_info *cursor);

/* ========================================================================= */
/* Info & Debug                                                              */
/* ========================================================================= */

/**
 * @brief Get last error message (human-readable).
 *
 * If ctx is NULL, returns a static error from the last drmtap_open() failure.
 *
 * @param ctx Capture context (may be NULL)
 * @return Error string (do not free), or NULL if no error
 */
const char *drmtap_error(drmtap_ctx *ctx);

/**
 * @brief Get GPU driver name.
 *
 * @param ctx Capture context
 * @return Driver name (e.g., "i915", "amdgpu", "nvidia", "virtio_gpu"),
 *         or NULL if not yet detected
 */
const char *drmtap_gpu_driver(drmtap_ctx *ctx);

/**
 * @brief Get the underlying DRM file descriptor.
 *
 * Useful for advanced operations like drmWaitVBlank() to synchronize
 * frame capture with display refresh. The fd is owned by the context —
 * do NOT close it.
 *
 * @param ctx Capture context
 * @return DRM fd (>= 0), or -1 if context is NULL
 */
int drmtap_drm_fd(drmtap_ctx *ctx);

/* ========================================================================= */
/* Pixel Conversion                                                          */
/* ========================================================================= */

/**
 * @brief Deswizzle tiled framebuffer data to linear.
 *
 * Converts GPU-tiled pixel data (Intel X/Y-tiled, Nvidia blocklinear)
 * to a linear row-by-row layout. The modifier tells which tiling format
 * to decode. Linear data (modifier == 0) is copied row-by-row.
 *
 * @param src        Source (tiled) pixel data
 * @param dst        Destination (linear) buffer (must be allocated by caller)
 * @param width      Frame width in pixels
 * @param height     Frame height in pixels
 * @param src_stride Source stride (bytes per row in tiled data)
 * @param dst_stride Destination stride (bytes per row)
 * @param modifier   DRM format modifier (e.g., I915_FORMAT_MOD_X_TILED)
 * @return 0 on success, negative errno on error
 */
int drmtap_deswizzle(const void *src, void *dst,
                     uint32_t width, uint32_t height,
                     uint32_t src_stride, uint32_t dst_stride,
                     uint64_t modifier);

/**
 * @brief Convert between pixel formats.
 *
 * Supported conversions:
 *   - XR30/AR30 (10-bit) → XRGB8888/ARGB8888
 *   - ABGR8888 → ARGB8888/XRGB8888
 *   - Same format → copy
 *
 * @param src        Source pixel data
 * @param dst        Destination buffer
 * @param width      Frame width in pixels
 * @param height     Frame height in pixels
 * @param src_stride Source stride (bytes per row)
 * @param dst_stride Destination stride (bytes per row)
 * @param src_format Source DRM fourcc (e.g., DRM_FORMAT_XRGB2101010)
 * @param dst_format Destination DRM fourcc (e.g., DRM_FORMAT_XRGB8888)
 * @return 0 on success, -ENOTSUP if conversion not supported
 */
int drmtap_convert_format(const void *src, void *dst,
                          uint32_t width, uint32_t height,
                          uint32_t src_stride, uint32_t dst_stride,
                          uint32_t src_format, uint32_t dst_format);

/* ========================================================================= */
/* Frame Differencing (optional utility)                                     */
/* ========================================================================= */

/** Rectangle describing a dirty region. */
typedef struct {
    uint32_t x, y, w, h;
} drmtap_rect;

/**
 * @brief Compare two frames and output changed rectangles.
 *
 * Pure CPU utility — scans memory blocks at tile_size granularity.
 * Useful for VNC/RDP servers that need dirty-rectangle encoding.
 *
 * @param frame_a   First frame (RGBA pixels)
 * @param frame_b   Second frame (same dimensions and format)
 * @param width     Frame width in pixels
 * @param height    Frame height in pixels
 * @param stride    Bytes per row
 * @param rects_out Output array for dirty rectangles
 * @param max_rects Maximum entries to write
 * @param tile_size Comparison granularity in pixels (e.g., 64)
 * @return Number of dirty rects (may be > max_rects), or negative errno
 */
int drmtap_diff_frames(const void *frame_a, const void *frame_b,
                       uint32_t width, uint32_t height, uint32_t stride,
                       drmtap_rect *rects_out, int max_rects,
                       int tile_size);

#ifdef __cplusplus
}
#endif

#endif /* DRMTAP_H */
