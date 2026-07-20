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

/* Version of the C library. Kept equal to the libdrmtap-sys crate version
 * (the C sources packaged for Rust are the same code) and to the meson
 * project version; the unit tests cross-check all three. The higher-level
 * `libdrmtap` Rust wrapper crate carries its own, separate version line. */
#define DRMTAP_VERSION_MAJOR 0
#define DRMTAP_VERSION_MINOR 4
#define DRMTAP_VERSION_PATCH 12

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
    uint32_t x;             /**< X offset in virtual FB (from CRTC) */
    uint32_t y;             /**< Y offset in virtual FB (from CRTC) */
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
/* Split capture: privileged export + unprivileged convert                   */
/* ========================================================================= */
/*
 * These entry points let a caller that already owns a privilege boundary
 * (e.g. a root service + an unprivileged worker) run the DRM export and the
 * GPU detile/convert in DIFFERENT processes, so the EGL/GLES and vendor GPU
 * driver stack never load into the privileged one.
 *
 *   privileged side:   drmtap_open() + drmtap_grab()  -> dma_buf_fd + metadata
 *                      (minimal DRM ops only; libEGL/libGLESv2 are dlopen'd
 *                       lazily on first convert, so a process that never
 *                       converts never maps the GPU stack at all)
 *   unprivileged side: drmtap_open_render() + drmtap_convert_dmabuf()
 *                      (imports the fd, EGL-detiles, converts to linear RGBA)
 *
 * The fd + metadata are carried between the two processes by the caller
 * (e.g. over a unix socket with SCM_RIGHTS) — libdrmtap does not impose a
 * wire format.
 */

/* Electro-optical transfer function of a scanout, from the connector
 * HDR_OUTPUT_METADATA infoframe (kernel/CTA-861 numbering). PQ means
 * "tone-map this"; HLG currently gets the plain bit-depth reduction
 * (PQ/HDR10 is the desktop norm). */
#define DRMTAP_EOTF_SDR  0  /**< traditional gamma SDR (or no HDR metadata) */
#define DRMTAP_EOTF_PQ   2  /**< SMPTE ST 2084 (HDR10) */
#define DRMTAP_EOTF_HLG  3  /**< Hybrid Log-Gamma (BT.2100) */

/**
 * @brief Descriptor of an externally-supplied scanout DMA-BUF.
 *
 * On the privileged exporter side use drmtap_grab_desc() to fill it in one
 * call; ship it (plus the dma_buf_fd) over IPC to the unprivileged converter.
 * num_planes/offsets/pitches carry the auxiliary planes of compressed layouts
 * (e.g. Intel CCS: main surface + CCS aux + clear-color) — these are NOT
 * present in drmtap_frame_info, so a split consumer must use drmtap_grab_desc
 * (not drmtap_grab alone) to capture a CCS or HDR scanout losslessly. All
 * planes live inside the one dma_buf_fd, as DRM GetFB2 reports them.
 */
typedef struct {
    int dma_buf_fd;         /**< Scanout DMA-BUF. May be -1 for an fb_id this
                                 context has already imported (see
                                 drmtap_convert_dmabuf). */
    uint32_t width;         /**< Frame width in pixels */
    uint32_t height;        /**< Frame height in pixels */
    uint32_t format;        /**< DRM fourcc of the scanout */
    uint64_t modifier;      /**< DRM format modifier (tiling/compression) */
    uint32_t fb_id;         /**< KMS framebuffer id — the import-once cache
                                 key. 0 disables caching for this frame. */
    uint32_t num_planes;    /**< Used entries in offsets/pitches (1..4);
                                 0 is treated as 1 */
    uint32_t offsets[4];    /**< Per-plane byte offsets into the DMA-BUF */
    uint32_t pitches[4];    /**< Per-plane strides in bytes; pitches[0] is the
                                 main surface stride */
    uint32_t hdr_eotf;      /**< DRMTAP_EOTF_*: PQ triggers the HDR->SDR
                                 tone-map during conversion */
    uint32_t hdr_max_nits;  /**< Mastering/content peak luminance (cd/m2),
                                 0 = unknown */
} drmtap_dmabuf_desc;

/**
 * @brief Capture a frame AND emit a complete DMA-BUF descriptor for the split.
 *
 * The privileged-exporter counterpart to drmtap_convert_dmabuf(): does a
 * zero-copy grab and fills @p desc with everything the unprivileged converter
 * needs — the dma_buf_fd, geometry, format/modifier, fb_id, the full plane
 * layout (num_planes/offsets/pitches, incl. Intel CCS aux planes) and the
 * connector HDR state (eotf/max_nits). drmtap_grab() alone cannot produce
 * these last two — drmtap_frame_info has no plane array or HDR fields — so a
 * split consumer that must handle compressed (CCS) or HDR scanouts has to use
 * this entry point.
 *
 * @p frame is also populated (same as drmtap_grab) and OWNS the resources:
 * release it with drmtap_frame_release() once the fd has been sent. @p desc is
 * a metadata snapshot safe to serialize over IPC, with ONE caveat: desc.dma_buf_fd
 * is an integer valid only in THIS (exporter) process — it aliases @p frame's fd.
 * Transfer the fd itself out of band (SCM_RIGHTS); on the receiving side the
 * converter must OVERWRITE desc.dma_buf_fd with the fd number it received before
 * calling drmtap_convert_dmabuf(), and owns/closes that received fd. Fails with
 * -ENOTSUP if the capture path produced pixels but no transferable dma-buf.
 *
 * @param ctx   Capture context (a real KMS context from drmtap_open)
 * @param desc  Output descriptor (value type; ship over IPC, fixing up the fd)
 * @param frame Output frame — owns the dma_buf_fd; release when done
 * @return 0 on success, negative errno on error
 */
int drmtap_grab_desc(drmtap_ctx *ctx, drmtap_dmabuf_desc *desc,
                     drmtap_frame_info *frame);

/**
 * @brief Open an UNPRIVILEGED, render-only context for drmtap_convert_dmabuf().
 *
 * Opens a DRM render node only. It does NOT open a KMS card, spawn the
 * privileged helper, or enumerate displays, and it needs no elevated
 * capability. Grab entry points return -ENOTSUP on this context.
 *
 * @param render_node Render node path (e.g. "/dev/dri/renderD128"),
 *                    or NULL to auto-select the first usable one.
 * @return Context handle, or NULL on error (call drmtap_error(NULL) for message)
 */
drmtap_ctx *drmtap_open_render(const char *render_node);

/**
 * @brief Convert an externally-supplied scanout frame to linear RGBA.
 *
 * On success frame->data points to context-owned pixels valid until the next
 * convert on this context or drmtap_close(). Do NOT free it and do NOT call
 * drmtap_frame_release() on it — there is nothing per-frame to release.
 * ALWAYS read the returned frame->format, ->stride and ->modifier to describe
 * the buffer:
 *   - the GPU (EGL) path normalizes every input to XRGB8888, stride width*4,
 *     modifier LINEAR;
 *   - the CPU fallback (used only when no EGL backend is available) also
 *     returns LINEAR width*4 pixels, but may keep the source 8-bit channel
 *     order (e.g. XBGR8888) in frame->format.
 * A well-behaved caller reads frame->format/->stride rather than assuming a
 * fixed layout.
 *
 * Import-once: the DMA-BUF import (EGLImage) is cached keyed by desc->fb_id
 * plus the buffer's identity (its dma-buf inode), so a fb_id the compositor
 * recycles onto a NEW buffer re-imports instead of serving stale pixels. The
 * compositor cycles a small pool of framebuffers, so after the first few
 * frames every convert hits the cache and no per-frame import happens. For a
 * known fb_id desc->dma_buf_fd may be -1; whenever the fb_id is new — or was
 * recycled — a valid fd must be supplied. The cached import holds its own
 * reference on the buffer, so the caller may close its fd right after the
 * call. DRMTAP_NO_IMAGE_CACHE=1 forces a fresh import per frame (debug aid).
 *
 * Threading: call convert AND drmtap_close for a given context from the SAME
 * thread. The EGL state and its cached imports are thread-local; closing on a
 * different thread cannot release them and would strand the cached buffers.
 *
 * When no GPU path is usable the conversion falls back to a CPU mmap +
 * deswizzle of the fd (same pipeline as the in-process grab). The descriptor
 * and fd are treated as untrusted: the geometry is validated, the fd MUST be a
 * genuine DMA-BUF (a non-dma-buf fd is rejected -- only an immutable dma-buf is
 * safe to mmap and read without a truncate-mid-read fault), and it must be
 * large enough to back the declared frame (offset + stride*height); anything
 * else returns -EINVAL rather than faulting.
 *
 * @param ctx   Context from drmtap_open_render() (or any context with a
 *              usable EGL backend)
 * @param desc  Input frame descriptor (metadata from the exporter)
 * @param frame Output frame (data/format/stride/modifier filled on return)
 * @return 0 on success, negative errno on error
 */
int drmtap_convert_dmabuf(drmtap_ctx *ctx, const drmtap_dmabuf_desc *desc,
                          drmtap_frame_info *frame);

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
 * @param src_size   Size of the source buffer in bytes; reads are bounded by it
 *                   (a scanout whose height is not a tile multiple would
 *                   otherwise index past stride*height)
 * @return 0 on success, negative errno on error
 */
int drmtap_deswizzle(const void *src, void *dst,
                     uint32_t width, uint32_t height,
                     uint32_t src_stride, uint32_t dst_stride,
                     uint64_t modifier, size_t src_size);

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

/**
 * @brief Tone-map an HDR10 (PQ, BT.2020) framebuffer to SDR 8-bit XRGB8888.
 *
 * Unlike drmtap_convert_format()'s naive bit-shift (which is only correct for
 * SDR 10-bit), this applies the real HDR10 transfer: PQ (SMPTE ST 2084) EOTF
 * to linear light, BT.2020 -> BT.709 gamut mapping, a highlight-preserving
 * tone-map down to the SDR range, and the sRGB OETF back to 8-bit. Use it only
 * when the scanout is actually HDR (decided from the connector Colorspace /
 * HDR_OUTPUT_METADATA, not from the pixel format alone — XR30/AR30 is also used
 * for plain SDR 10-bit).
 *
 * Supported src_format: XR30/AR30 (ARGB2101010). Others return -ENOTSUP.
 *
 * @param src        Source pixel data
 * @param dst        Destination buffer (XRGB8888)
 * @param width      Frame width in pixels
 * @param height     Frame height in pixels
 * @param src_stride Source stride (bytes per row)
 * @param dst_stride Destination stride (bytes per row)
 * @param src_format Source DRM fourcc (DRM_FORMAT_XRGB2101010 / ARGB2101010)
 * @param max_nits   Content/mastering peak luminance for the highlight roll-off
 *                   (0 = a sensible default). Brighter peaks spread highlights
 *                   over more of the top range instead of clipping to white.
 * @return 0 on success, -ENOTSUP for an unsupported format, -EINVAL on bad args
 */
int drmtap_tonemap_hdr10(const void *src, void *dst,
                         uint32_t width, uint32_t height,
                         uint32_t src_stride, uint32_t dst_stride,
                         uint32_t src_format, uint32_t max_nits);

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
