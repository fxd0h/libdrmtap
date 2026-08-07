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

/* Per-dimension ceiling, used to make width*height wrap-proof before the multiply.
 * Far above any real scanout; the byte cap above is what actually binds. */
#define DRMTAP_MAX_DIM 32768u

struct drmtap_ctx {
    /* DRM device */
    int drm_fd;
    char device_path[256];
    char driver_name[64];
    /* Render node of THIS device, resolved lazily by drmtap_render_node().
     * Empty until first asked (the device has none, or it was never queried). */
    char render_node[256];

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
        /* The framebuffer OBJECT width, kept alongside `width` (which is the
         * SCANNED-OUT width and may be narrower on a padded scanout). Transfer and
         * allocation geometry must use this one: a virtio transfer box describes the
         * buffer being made coherent, not the visible sub-region of it. */
        uint32_t fb_width;
        uint32_t format;
        uint64_t modifier;
        /* Full plane layout captured at cache-miss (GetFB2) time. Restaged
         * into ctx->fb2_* on every cache HIT so the EGL detile / CCS import
         * sees THIS fb's planes, not whatever the last GetFB2 left there. */
        int      fb2_num_planes;
        uint32_t fb2_pitches[4];
        uint32_t fb2_offsets[4];
        /* 1 = a DMA_BUF_SYNC_START is open on prime_fd and still owes its END.
         * The fast path keeps its mapping across frames, so the CPU-access
         * window closes at the next grab, not at frame release -- a fast frame
         * carries no _priv and is never released. Without this flag each slot
         * accumulated one unmatched START per frame for its whole lifetime. */
        int      sync_started;
    } fast_slots[4];
    uint32_t fast_plane_id;         /* cached primary plane id */
    uint32_t fast_last_fb_id;       /* fb_id from last capture (change detect) */
    int      fast_initialized;      /* 1 = plane found, slots ready */
    /* 1 = this device's scanout refused a CPU mmap, so the fast path serves
     * every frame through the EGL fd fallback and caches no slot. Sticky per
     * context: whether a scanout BO is CPU-mappable is a property of the driver
     * and the placement (amdgpu GFX9+ keeps it in VRAM), not of one frame, so
     * retrying the mmap on every frame only buys a failing syscall. Also makes
     * the per-frame "miss" honest in the log: nothing was ever cached to hit. */
    int      fast_no_cpu_map;

    /* Set once drmtap_grab_mapped_fast has established that this process cannot
     * export the scanout itself (drmModeGetFB2 returns handles[0]==0 without
     * CAP_SYS_ADMIN). Unlike drmtap_grab_mapped, the fast path has no helper
     * fallback -- caching a persistent CPU mapping per framebuffer is its whole
     * purpose, and a helper hands over a fresh fd per grab, so there is nothing
     * stable to cache. It therefore cannot work for an unprivileged caller on
     * any GPU. Sticky so the diagnosis is stated once instead of once per frame:
     * reported as pages of "CACHE MISS ... cold start" with the real reason only
     * in drmtap_error(), it read as a broken cache (issue #36). */
    int      fast_no_privilege;

    /* Set once the CRTC mode and the scanout framebuffer have been found to
     * disagree on width, so the reason a frame is narrower than the fb (or is
     * deliberately NOT narrowed) is stated once per context instead of per
     * frame. See drmtap_scanout_width() in drm_grab.c. */
    int      logged_scanout_crop;

    /* 1 = DRM_CLIENT_CAP_ATOMIC has been requested on drm_fd. Set LAZILY, only when
     * a scanout framebuffer turns out to be wider than its mode, because the plane
     * SRC_W/CRTC_W properties that settle whether that is pitch padding or a scaling
     * plane are hidden from a non-atomic client (measured: the same plane reports
     * SRC_W with the cap and nothing without it). Per-fd, needs no privilege, no
     * atomic commit is ever made, and no other client is affected. Sticky so the
     * cap is requested once rather than per frame. */
    int      atomic_cap_tried;

    /* Memoized scanout-width decision for a PADDED scanout. Reading the plane rect
     * costs a plane-resource sweep plus one drmModeGetProperty per property, and the
     * answer only changes when the CRTC, the mode, the framebuffer width or the
     * layout changes -- not per page flip. Keyed on all four so a modeset invalidates
     * it (a mode change with an identical fb width and modifier would otherwise be
     * served the stale answer). The cheap drmModeGetCrtc gate still runs per grab: it
     * is the ioctl the code always did, and it supplies hdisplay for this key. */
    uint32_t sw_key_crtc;
    uint32_t sw_key_hdisplay;
    uint32_t sw_key_fb_width;
    uint64_t sw_key_modifier;
    uint32_t sw_cached_width;
    int      sw_cached;

    /* Caller-supplied destination for converted pixels (drmtap_set_output_buffer).
     * NULL means "use the ctx-owned deswizzle_buf". Set once by the caller, who
     * owns the memory and must keep it alive; libdrmtap never frees or reallocates
     * it, and refuses a frame that would not fit rather than writing short. Every
     * path that produces converted pixels resolves its destination through
     * drmtap_ensure_out(), so the EGL detile and the CPU conversions behave
     * identically -- a caller must not have to know which one ran. */
    void   *user_out;
    size_t  user_out_len;

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

/* Debug log to stderr, only when ctx->debug is set. Takes a CONST ctx so a
 * read-only helper can still report -- gpu_egl.c used to pass NULL from twenty
 * sites for want of that, which silenced every EGL failure diagnostic it had. */
void drmtap_debug_log(const drmtap_ctx *ctx, const char *fmt, ...);

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

/* Resolve where this frame's converted pixels must be written: the caller's
 * buffer when drmtap_set_output_buffer() set one (checked against `size`, so a
 * geometry change that no longer fits fails the grab instead of writing short or
 * overflowing), otherwise the ctx-owned grow-once deswizzle buffer. Returns 0 and
 * sets *out, or -ENOSPC / whatever drmtap_ensure_buf returns. Every producer of
 * converted pixels must go through this and must NOT reference
 * ctx->deswizzle_buf directly, or the caller's buffer would be honoured on some
 * paths and silently ignored on others. (drm_grab.c) */
int drmtap_ensure_out(drmtap_ctx *ctx, size_t size, void **out);

/* Reject framebuffer DIMENSIONS that cannot be a real scanout, before anything
 * multiplies them. validate_fb_size() bounds stride*height, which does NOT bound
 * width: a wire or IPC peer that sends a huge width with a small stride passes it,
 * and every converted output is sized width*height*4 -- a product that can wrap
 * size_t and hand a large write a small destination. Returns 0, -EINVAL (a zero
 * dimension) or -EFBIG (a dimension over DRMTAP_MAX_DIM, or more than
 * DRMTAP_MAX_FB_BYTES worth of pixels). (drm_grab.c) */
int drmtap_validate_fb_dims(uint32_t width, uint32_t height);

/* GPU backend: EGL/GLES2 universal detiling (gpu_egl.c).
 * On success *out_data points at the resolved conversion destination: the caller's
 * output buffer when drmtap_set_output_buffer() set one, otherwise ctx->deswizzle_buf
 * (ctx-owned, grow-once, valid until the next convert or drmtap_close).
 * The caller must NOT free it. fb_id keys the import-once EGLImage cache (0 = no caching); for an
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

/* Which branch drmtap_scanout_width_of() took, so the caller can log the reason
 * once and a test can assert the branch and not just the number. */
typedef enum {
    DRMTAP_SCANOUT_AS_IS = 0,           /* fb reported unchanged */
    DRMTAP_SCANOUT_NARROWED,            /* padded fb narrowed to what is scanned out */
    DRMTAP_SCANOUT_OFFSET_UNSUPPORTED,  /* plane reads from an offset in a bigger fb */
    DRMTAP_SCANOUT_TILED_NOT_NARROWED,  /* padded, but tiled: width feeds tile math */
    DRMTAP_SCANOUT_SCALING_NOT_NARROWED,/* plane genuinely scales; fb is all visible */
    DRMTAP_SCANOUT_NO_PLANE_RECT,       /* plane rect unreadable: cannot tell, so do not */
} drmtap_scanout_why;

/* The primary plane rectangle: which region of the framebuffer the plane reads and
 * where it lands on the CRTC. Whole pixels (the kernel SRC_* properties are 16.16
 * fixed point; the fractional part is dropped by the reader). `valid` is 0 when the
 * properties could not be read, which is a distinct case from "no scaling" and must
 * not be treated as one. */
typedef struct {
    int      valid;
    uint32_t src_x, src_y, src_w, src_h;
    uint32_t crtc_w, crtc_h;
} drmtap_plane_rect;

/* Decide the width a CRTC actually scans out of a framebuffer that may be wider
 * than its mode. Pure (no DRM fd) so it is testable without the padding hardware.
 * See the comment on the definition in drm_grab.c for the rules. */
uint32_t drmtap_scanout_width_of(uint32_t fb_width,
                                 const drmtap_plane_rect *rect,
                                 int layout_is_linear,
                                 drmtap_scanout_why *why);

/* Kernel spelling of a DRM connector type ("eDP", "HDMI-A", "LVDS", ...), the
 * prefix of drmtap_display.name. "Unknown" for a type this build does not know.
 * Pure, so the whole table is testable without the matching hardware. */
const char *drmtap_connector_type_name(uint32_t connector_type);

#endif /* DRMTAP_INTERNAL_H */
