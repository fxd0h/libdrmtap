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

/* Upper bound on any CPU-side framebuffer buffer we allocate from a GetFB2
 * report. Doubles as a DoS guard: a bogus or hostile width/height/stride
 * (the framebuffer geometry is not under our control) cannot force an
 * unbounded allocation. Sized for one 8K BGRA frame (7680 x 4320 x 4 bytes,
 * ~126 MB). */
#define DRMTAP_MAX_FB_BYTES ((size_t)7680 * 4320 * 4)

struct drmtap_ctx {
    /* DRM device */
    int drm_fd;
    char device_path[256];
    char driver_name[64];

    /* Selected display */
    uint32_t crtc_id;

    /* 1 = context from drmtap_open_render(): render node only, no KMS.
     * Grab entry points reject it; only drmtap_convert_dmabuf() applies. */
    int is_render_only;

    /* Cached resources for hotplug detection */
    uint64_t cached_topology_hash;  /* per-connector state fold; hotplug/modeset signal */

    /* Helper binary */
    char helper_path[512];
    int helper_pid;
    int helper_fd;          /* socket to helper */

    /* Error handling */
    char error_msg[512];

    /* Debug */
    int debug;

    /* ── Persistent fast-grab state (double-buffer cache) ── */
    /* Cache up to 4 buffer slots indexed by fb_id.
     * Compositor typically uses 2-3 buffers (double/triple buffering).
     * Each slot keeps its GEM handle + mmap alive across frames,
     * eliminating GetFB2 + PrimeHandleToFD + mmap/munmap per frame. */
    #define DRMTAP_FAST_SLOTS 4
    struct {
        uint32_t fb_id;         /* KMS framebuffer id (0 = slot unused) */
        uint32_t gem_handle;    /* GEM handle from GetFB2 import */
        int      prime_fd;      /* DMA-BUF fd */
        void    *mmap_ptr;      /* persistent mmap */
        size_t   mmap_size;     /* mapped region size */
        uint32_t width, height, stride;
        uint32_t format;
        uint64_t modifier;
        /* Full plane layout captured at cache-miss (GetFB2) time. Restaged
         * into ctx->fb2_* on every cache HIT so the EGL detile / CCS import
         * sees THIS fb's planes, not whatever the last GetFB2 left there. */
        int      fb2_num_planes;
        uint32_t fb2_pitches[4];
        uint32_t fb2_offsets[4];
    } fast_slots[4];
    uint32_t fast_plane_id;         /* cached primary plane id */
    uint32_t fast_last_fb_id;       /* fb_id from last capture (change detect) */
    int      fast_initialized;      /* 1 = plane found, slots ready */

    /* Deswizzle shadow buffer (for read-only mmap'd DMA-BUFs).
     * Grow-once and reused across grabs; capped at DRMTAP_MAX_FB_BYTES;
     * freed in drmtap_close(). */
    void *deswizzle_buf;
    size_t deswizzle_buf_size;

    /* Helper-mode (V2) pixel receive buffer. Same model as deswizzle_buf:
     * ctx-owned, grow-once, reused across grabs, capped, freed in
     * drmtap_close() — never a per-frame malloc/free. */
    void *pixel_buf;
    size_t pixel_buf_size;

    /* Cached FB2 multi-plane info (for EGL CCS import) */
    uint32_t fb2_pitches[4];
    uint32_t fb2_offsets[4];
    int      fb2_num_planes;  /* number of active planes (1..4) */

    /* HDR state of the frame currently being processed. Set per grab from the
     * connector HDR_OUTPUT_METADATA (helper sends it on the wire; direct mode
     * reads it itself) and consumed by the conversion path to decide whether to
     * tone-map (DRMTAP_EOTF_PQ) or do a plain bit-depth reduction. */
    uint32_t cur_hdr_eotf;     /* DRMTAP_EOTF_* */
    uint32_t cur_hdr_max_nits; /* peak luminance, 0 = unknown */
};

/* ========================================================================= */
/* Internal API (used across modules)                                        */
/* ========================================================================= */

// Set error message on context (or global static if ctx is NULL)
void drmtap_set_error(drmtap_ctx *ctx, const char *fmt, ...);

// Debug log to stderr (only when ctx->debug is set)
void drmtap_debug_log(drmtap_ctx *ctx, const char *fmt, ...);

/* The command frame (helper_cmd_grab_t), its CMD_* types and the magic/version
 * validation are defined in wire.h, shared with the helper so the two ends of
 * the protocol cannot drift. The library client includes wire.h directly. */

/* Result from helper v2 grab — helper reads pixels and sends via socket.
 * Must match struct grab_metadata in drmtap-helper.c */
/* DRMTAP_EOTF_* moved to the public header (drmtap.h): the split-capture
 * descriptor carries the scanout EOTF across the process boundary. */

/* Flags for helper_grab_result_t.flags */
#define HELPER_FLAG_HAS_DMABUF  0x01  /* DMA-BUF fd follows via SCM_RIGHTS */
#define HELPER_FLAG_VIRGL       0x02  /* DMA-BUF is a host-rendered virgl scanout:
                                       * read it back on the GPU (EGL), not via a
                                       * CPU mmap (which is black for it) */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t fb_id;
    uint32_t data_size;     /* 0 = error, >0 = pixel data follows (unless FLAG_HAS_DMABUF) */
    uint64_t modifier;
    uint32_t seq;           /* frame sequence number from helper */
    uint64_t timestamp_ms;  /* unix ms when helper read the frame */
    uint32_t flags;         /* HELPER_FLAG_* bits */
    uint32_t hdr_eotf;      /* DRM EOTF of the scanout: 0=SDR, 2=PQ (ST2084), 3=HLG */
    uint32_t hdr_max_nits;  /* mastering/content peak luminance (cd/m2), 0=unknown */
} helper_grab_wire_t;  /* wire-format: this is what goes over the socket */

typedef struct {
    helper_grab_wire_t wire;  /* wire-format fields */
    int dmabuf_fd;            /* DMA-BUF fd received via SCM_RIGHTS (-1 if none) */
} helper_grab_result_t;

/* Helper lifecycle (privilege_helper.c) */
int drmtap_helper_spawn(drmtap_ctx *ctx);
void drmtap_helper_stop(drmtap_ctx *ctx);
/* Fast-grab persistent state cleanup (drm_grab.c) */
void drmtap_fast_cleanup(drmtap_ctx *ctx);

int drmtap_helper_grab(drmtap_ctx *ctx, helper_grab_result_t *result,
                        void *pixel_buf, size_t buf_size);

/* Cursor metadata received from the helper — must match struct cursor_metadata
 * in drmtap-helper.c. */
typedef struct {
    int32_t  x, y;
    int32_t  hot_x, hot_y;
    uint32_t width, height;
    uint32_t visible;
    uint32_t data_size;
} helper_cursor_wire_t;

/* Capture the cursor via the privileged helper (used when the library process
 * lacks CAP_SYS_ADMIN). Populates `cursor` (allocates cursor->pixels). */
int drmtap_helper_get_cursor(drmtap_ctx *ctx, drmtap_cursor_info *cursor);

/* GPU backend: generic linear (gpu_generic.c) */
int drmtap_gpu_generic_match(const char *driver);
int drmtap_gpu_generic_process(drmtap_ctx *ctx, void *data,
                               uint32_t width, uint32_t height,
                               uint32_t stride, uint32_t format,
                               uint64_t modifier);

/* GPU backend: Intel i915/xe (gpu_intel.c) */
int drmtap_gpu_intel_match(const char *driver);
int drmtap_gpu_intel_process(drmtap_ctx *ctx, void *data,
                             uint32_t width, uint32_t height,
                             uint32_t stride, uint32_t format,
                             uint64_t modifier);

/* GPU backend: AMD amdgpu (gpu_amd.c) */
int drmtap_gpu_amd_match(const char *driver);
int drmtap_gpu_amd_process(drmtap_ctx *ctx, void *data,
                           uint32_t width, uint32_t height,
                           uint32_t stride, uint32_t format,
                           uint64_t modifier);

/* GPU backend: Nvidia (gpu_nvidia.c) */
int drmtap_gpu_nvidia_match(const char *driver);
int drmtap_gpu_nvidia_process(drmtap_ctx *ctx, void *data,
                              uint32_t width, uint32_t height,
                              uint32_t stride, uint32_t format,
                              uint64_t modifier);

/* Ensure *buf holds at least `size` bytes: grow-once, never shrinks, capped
 * at DRMTAP_MAX_FB_BYTES. Contents are not preserved across a grow. Returns
 * 0, -EINVAL (zero size), -EFBIG (over the cap) or -ENOMEM. (drm_grab.c) */
int drmtap_ensure_buf(void **buf, size_t *cap, size_t size);

/* GPU backend: EGL/GLES2 universal detiling (gpu_egl.c).
 * On success *out_data points at ctx->deswizzle_buf (ctx-owned, grow-once,
 * valid until the next convert or drmtap_close) — the caller must NOT free
 * it. fb_id keys the import-once EGLImage cache (0 = no caching); for an
 * fb_id already cached with matching geometry dma_buf_fd may be -1. */
int drmtap_gpu_egl_available(drmtap_ctx *ctx);
int drmtap_gpu_egl_convert(drmtap_ctx *ctx,
                            int dma_buf_fd,
                            uint32_t width, uint32_t height,
                            uint32_t stride, uint32_t fourcc,
                            uint64_t modifier, uint32_t fb_id,
                            void **out_data, size_t *out_size);

/* Release the calling thread's lazily-built EGL detile context + GL resources.
 * Must be called on the capture thread (drmtap_close does this) — C thread-local
 * storage has no destructor, so without it every open/close on a fresh thread
 * leaks a whole EGL context + linear texture. No-op if this thread never detiled
 * or the library was built without EGL. Never terminates the shared display. */
void drmtap_gpu_egl_thread_cleanup(void);

/* Convert a 16-bit/channel scanout (XR48/AR48/XB48/AB48) to XRGB8888.
 * bgr selects channel order (0 = XR48/AR48, 1 = XB48/AB48). When eotf is
 * DRMTAP_EOTF_PQ the 16-bit values are PQ-decoded and tone-mapped to SDR;
 * otherwise they are reduced to 8-bit directly. (pixel_convert.c) */
int drmtap_convert_rgb16(const void *src, void *dst,
                         uint32_t width, uint32_t height,
                         uint32_t src_stride, uint32_t dst_stride,
                         int bgr, uint32_t eotf, uint32_t max_nits);

/* Reduce a half-float (FP16) scanout -- XRGB16161616F ('XR4H') and its BGR/alpha
 * siblings, 8 bytes/pixel -- to 8-bit XRGB8888. FP16 carries linear light, decoded
 * to linear then re-encoded through the sRGB OETF; HDR highlights (>1.0) clip.
 * `bgr` selects channel order. (pixel_convert.c) */
int drmtap_convert_rgb16f(const void *src, void *dst,
                          uint32_t width, uint32_t height,
                          uint32_t src_stride, uint32_t dst_stride, int bgr);

#endif /* DRMTAP_INTERNAL_H */
