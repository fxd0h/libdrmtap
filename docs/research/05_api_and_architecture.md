# API Comparison and Proposed libdrmtap Architecture

> **Date**: 2026-03-14 (updated)  
> **Synthesis of**: 00_landscape through 08_reframe_egl_analysis

---

## Existing API Comparison

| Aspect | FFmpeg kmsgrab | kmsvnc | kms-screenshot | libdrmtap (proposed) |
|---|---|---|---|---|
| Type | Capture device | VNC server | CLI tool | **C Library** |
| Language | C | C | C | C |
| Public API | FFmpeg options | main() | main() | **C functions** |
| Embeddable | ❌ (needs FFmpeg) | ❌ (monolithic) | ❌ (monolithic) | **✅** |
| Permissions | Direct CAP_SYS_ADMIN | — | Direct root | **Automatic helper** |
| Intel/AMD | ✅ (via hwframes) | ✅ (VAAPI) | ✅ (AMDGPU SDMA) | **✅ (EGL + CPU fallback)** |
| Nvidia | ⚠️ (limited) | ⚠️ (x-tiled) | ❌ | **✅ (EGL + deswizzle)** |
| VM | ✅ | ✅ | ❌ | **✅** |
| Multi-monitor | ✅ (per CRTC) | ✅ (per CRTC) | ✅ | **✅** |
| Cursor capture | ❌ | ✅ | ❌ | **✅** |
| HDR | ❌ | ❌ | ✅ (tone-map) | **✅** |
| Continuous capture | ✅ (frames/sec) | ✅ (VNC stream) | ❌ (1 shot) | **✅** |
| Output formats | DRM_PRIME fd | RGBA buffer | PPM file | **DMA-BUF fd + mmap** |
| Dependencies | libavutil,libdrm | libdrm,libva,libvncserver | libdrm,libamdgpu,vulkan | **libdrm, egl/glesv2(opt)** |

---

## Proposed Architecture for libdrmtap

### Layers

```
┌─────────────────────────────────────────────────────────┐
│                    APPLICATION                           │
│         (RustDesk, Sunshine, VNC client, etc.)           │
├─────────────────────────────────────────────────────────┤
│                    libdrmtap.h                           │
│             Public API — 10-15 functions                 │
├─────────────────────────────────────────────────────────┤
│                                                          │
│  ┌──────────────┐  ┌──────────┐  ┌───────────────────┐  │
│  │ drm_enumerate│  │ drm_grab │  │ pixel_convert     │  │
│  │              │  │          │  │                   │  │
│  │ Planes/CRTCs │  │ GetFB2   │  │ Tiling→Linear     │  │
│  │ Connectors   │  │ Prime FD │  │ Format conversion │  │
│  │ Capabilities │  │ DMA-BUF  │  │ HDR tone mapping  │  │
│  └──────────────┘  └──────────┘  └───────────────────┘  │
│                                                          │
│  ┌──────────────┐  ┌──────────────────────────────────┐  │
│  │ gpu_backend  │  │ privilege_helper                 │  │
│  │              │  │                                  │  │
│  │ EGL: All GPUs│  │ Auto-spawn drmtap-helper         │  │
│  │ Intel: CPU   │  │ SCM_RIGHTS fd passing            │  │
│  │ AMD:   CPU   │  │ cap_drop + seccomp               │  │
│  │ Nvidia: CPU  │  │ Transparent fallback              │  │
│  │ VM: Direct   │  │                                  │  │
│  └──────────────┘  └──────────────────────────────────┘  │
│                                                          │
├─────────────────────────────────────────────────────────┤
│              libdrm / kernel DRM/KMS                     │
└─────────────────────────────────────────────────────────┘
```

### Proposed API (headers)

```c
// === drmtap.h — Public API ===

#include <stdint.h>

// Opaque handles
typedef struct drmtap_ctx drmtap_ctx;
typedef struct drmtap_frame drmtap_frame;

// Display information
typedef struct {
    uint32_t crtc_id;
    uint32_t connector_id;
    uint32_t plane_id;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_hz;
    uint32_t pixel_format;  // DRM fourcc
    char name[32];          // "HDMI-A-1", "DP-1", etc.
} drmtap_display;

// Capture configuration
typedef struct {
    const char *device_path;    // NULL = auto-detect
    uint32_t crtc_id;           // 0 = primary display
    int capture_cursor;         // 1 = include cursor
    const char *helper_path;    // NULL = auto-find "drmtap-helper"
} drmtap_config;

// Captured frame
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t format;         // DRM fourcc
    uint32_t stride;         // bytes per row
    int dma_buf_fd;          // DMA-BUF fd (zero-copy)
    void *data;              // mmap'd pixel data (if requested)
    size_t data_size;
    uint64_t timestamp_ns;
} drmtap_frame_info;

// === Functions ===

// Lifecycle
drmtap_ctx *drmtap_open(const drmtap_config *config);
void drmtap_close(drmtap_ctx *ctx);

// Enumeration
int drmtap_list_displays(drmtap_ctx *ctx, drmtap_display *out, int max_count);

// Capture — zero-copy: returns DMA-BUF fd
int drmtap_grab(drmtap_ctx *ctx, drmtap_frame_info *frame);

// Capture — with mmap: returns pointer to linear RGBA data
int drmtap_grab_mapped(drmtap_ctx *ctx, drmtap_frame_info *frame);

// Release
void drmtap_frame_release(drmtap_ctx *ctx, drmtap_frame_info *frame);

// Info & debug
const char *drmtap_error(drmtap_ctx *ctx);
const char *drmtap_gpu_driver(drmtap_ctx *ctx);
int drmtap_version(void);

// --- Cursor Capture (v1) ---
// Cursor is returned SEPARATELY from the framebuffer.
// Remote desktop clients render cursor on the client side for lower latency.

typedef struct {
    int32_t x, y;           // cursor position on screen
    int32_t hot_x, hot_y;   // hotspot within cursor image
    uint32_t width, height;
    uint32_t *pixels;       // ARGB8888, pre-multiplied alpha (NULL if hidden)
    int visible;            // 0 = cursor hidden
} drmtap_cursor_info;

// Get current cursor state (position + image)
// Returns 0 on success, -ENOENT if no cursor plane, negative errno on error
int drmtap_get_cursor(drmtap_ctx *ctx, drmtap_cursor_info *cursor);
void drmtap_cursor_release(drmtap_ctx *ctx, drmtap_cursor_info *cursor);

// --- Display Hotplug Detection (v1) ---
// Call after drmtap_grab*() returns -ENODEV or periodically to detect changes.
// Returns 1 if display configuration changed since last call, 0 if unchanged.
int drmtap_displays_changed(drmtap_ctx *ctx);

// --- HDR Metadata (v2) ---
// Available when the framebuffer uses a 10-bit or HDR format.

typedef struct {
    uint32_t colorspace;        // DRM_MODE_COLORIMETRY_*
    uint32_t transfer_function; // PQ, HLG, linear, sRGB
    uint16_t max_cll;           // max content light level (cd/m²)
    uint16_t max_fall;          // max frame-average light level
    // Mastering display info
    struct {
        uint16_t r_x, r_y;     // red primary (x, y in 0.00002 units)
        uint16_t g_x, g_y;
        uint16_t b_x, b_y;
        uint16_t w_x, w_y;     // white point
        uint32_t min_lum;      // min luminance (0.0001 cd/m²)
        uint32_t max_lum;      // max luminance (cd/m²)
    } mastering;
    int valid;                  // 0 = no HDR metadata available
} drmtap_hdr_info;

// Get HDR metadata for the current display
// Returns 0 on success, -ENODATA if SDR, negative errno on error
int drmtap_get_hdr_info(drmtap_ctx *ctx, drmtap_hdr_info *hdr);
```

### Typical Usage Flow

```c
// Minimal example: capture one frame as RGBA
#include <drmtap.h>

int main() {
    drmtap_config cfg = {0};  // defaults
    drmtap_ctx *ctx = drmtap_open(&cfg);
    if (!ctx) { fprintf(stderr, "init failed\n"); return 1; }
    
    printf("GPU: %s\n", drmtap_gpu_driver(ctx));
    
    // List displays
    drmtap_display displays[8];
    int n = drmtap_list_displays(ctx, displays, 8);
    for (int i = 0; i < n; i++)
        printf("  %s: %dx%d@%dHz\n", displays[i].name,
               displays[i].width, displays[i].height,
               displays[i].refresh_hz);
    
    // Capture
    drmtap_frame_info frame;
    if (drmtap_grab_mapped(ctx, &frame) == 0) {
        // frame.data contains linear RGBA
        // frame.width, frame.height, frame.stride available
        save_as_png(frame.data, frame.width, frame.height, frame.stride);
        drmtap_frame_release(ctx, &frame);
    }
    
    drmtap_close(ctx);
}
```

### Zero-Copy Flow (for RustDesk, Sunshine)

```c
// Zero-copy capture: only get DMA-BUF fd
// Ideal for passing to VAAPI encoder or Vulkan directly
drmtap_frame_info frame;
if (drmtap_grab(ctx, &frame) == 0) {
    // frame.dma_buf_fd is a DMA-BUF file descriptor
    // Pass directly to video encoder:
    vaapi_encode_from_dmabuf(frame.dma_buf_fd, frame.width, frame.height);
    drmtap_frame_release(ctx, &frame);
}
```

### Continuous Capture (for Remote Desktop / Streaming)

Continuous capture requires no special API — call `drmtap_grab_mapped()` or `drmtap_grab()` in a loop. This is how all existing DRM capture tools work (kmsvnc, gpu-screen-recorder, FFmpeg kmsgrab).

```c
// Continuous capture at ~60fps
volatile int running = 1;

void capture_loop(drmtap_ctx *ctx) {
    drmtap_frame_info frame;
    
    while (running) {
        int ret = drmtap_grab_mapped(ctx, &frame);
        if (ret < 0) {
            fprintf(stderr, "Capture failed: %s\n", drmtap_error(ctx));
            usleep(100000);  // retry after 100ms on error
            continue;
        }
        
        // Process the frame (encode, send, etc.)
        encode_and_send(frame.data, frame.width, frame.height, frame.stride);
        
        drmtap_frame_release(ctx, &frame);
        
        usleep(16666);  // ~60fps cap (or use vblank sync)
    }
}
```

**Key design decision**: We do NOT provide callback-based or epoll-based APIs. A simple polling loop is:
- Easier to understand and debug
- Trivial to wrap in any threading model (pthreads, tokio, glib main loop)
- How kmsvnc, gpu-screen-recorder, and FFmpeg all work internally
- Sufficient for 30-120fps capture rates

### Why There's No Vblank Sync API (Yet)

DRM vblank events (`drmCrtcQueueSequence`, `DRM_EVENT_VBLANK`) are available for syncing to the display refresh. However:
- They require DRM Master or `CAP_SYS_ADMIN` — the helper would need to relay events
- Most integrators (RustDesk, Sunshine) have their own frame pacing
- Adding complexity without clear benefit for v1

This may be added in v2 as an optional `drmtap_wait_vblank(ctx)` that blocks until the next refresh.

---

## Damage Tracking and Frame Differencing

### Who is Responsible for What?

```
┌─────────────────────────────────────────────┐
│  libdrmtap (our responsibility)             │
│                                              │
│  ✅ Deliver the complete framebuffer         │
│  ✅ Convert tiled → linear (if needed)       │
│  ✅ Provide timestamp per frame              │
│  ✅ Handle permissions transparently         │
│  ✅ Detect changed regions (dirty rects)     │
│                                              │
│  ❌ NOT: encode/compress                     │
│  ❌ NOT: network transport                   │
└─────────────────────────────────────────────┘
                    │
                    │ complete framebuffer (always)
                    ▼
┌─────────────────────────────────────────────┐
│  Integrating Application (their job)         │
│                                              │
│  VNC server:  memcmp regions → send dirty    │
│  RustDesk:    frame diff → encode changes    │
│  Sunshine:    VAAPI encoder handles it       │
│  OBS:        codec inter-frame prediction    │
└─────────────────────────────────────────────┘
```

### Why We Don't Provide Damage Tracking

**`FB_DAMAGE_CLIPS` is for writers, not readers.**

The kernel's `FB_DAMAGE_CLIPS` plane property allows the **compositor** (the writer) to tell the kernel "I only changed these rectangles." This is an optimization for virtual display drivers (VMs, remote framebuffers) to avoid redrawing the entire display.

As a **reader** calling `drmModeGetFB2()`, we always get the current framebuffer — complete, with no damage information. There is no kernel API that tells a reader "these regions changed since your last read."

### How Existing Projects Handle This

| Project | Damage detection method | Where |
|---|---|---|
| **kmsvnc** | `memcmp()` tiles between frames | Client-side (the VNC server) |
| **RustDesk** | Frame diff in encoder pipeline | Client-side |
| **Sunshine** | H.264/H.265 inter-frame prediction | Codec handles it |
| **gpu-screen-recorder** | H.264/H.265 encoder | Codec handles it |
| **FFmpeg kmsgrab** | Codec handles it | Encoder |

**Conclusion**: Every project that needs damage tracking does it at the application layer, not the capture layer. This is the correct separation of concerns.

### Frame Differencing API (Implemented)

For applications that want basic frame differencing without implementing their own, libdrmtap provides a utility function:

```c
// Compare two frames and output changed rectangles
typedef struct {
    uint32_t x, y, w, h;
} drmtap_rect;

int drmtap_diff_frames(const void *frame_a, const void *frame_b,
                       uint32_t width, uint32_t height, uint32_t stride,
                       drmtap_rect *rects_out, int max_rects,
                       int tile_size);  // comparison granularity (e.g., 64x64)
// Returns: number of dirty rectangles found, or negative errno on error
```

This is a pure CPU utility — it compares memory blocks at tile granularity. Useful for VNC/RDP servers that need dirty-rectangle encoding.

---

## Project File Map

```
libdrmtap/
├── include/
│   └── drmtap.h                 # Public API
├── src/
│   ├── drmtap.c                 # Main context management
│   ├── drm_enumerate.c          # Plane/CRTC enumeration
│   ├── drm_grab.c               # Framebuffer capture
│   ├── pixel_convert.c          # Deswizzle + format conversion
│   ├── gpu_egl.c                # EGL/GLES2 universal detiling (all GPUs)
│   ├── gpu_intel.c              # Intel CPU deswizzle fallback
│   ├── gpu_amd.c                # AMD CPU deswizzle fallback
│   ├── gpu_nvidia.c             # Nvidia CPU deswizzle fallback
│   ├── gpu_generic.c            # Generic/VM backend (linear)
│   ├── cursor.c                 # Hardware cursor capture
│   └── privilege_helper.c       # Helper spawn + communication
├── helper/
│   └── drmtap-helper.c          # Privileged binary (~500 lines)
├── tests/
│   ├── test_enumerate.c         # Tests with vkms
│   ├── test_capture.c           # Frame capture integration tests
│   ├── test_formats.c           # Pixel format conversion tests
│   ├── test_helper.c            # Helper IPC tests
│   └── test_deswizzle.c         # Tiling conversion unit tests
├── bindings/rust/
│   ├── libdrmtap-sys/           # Raw FFI bindings (crates.io)
│   └── libdrmtap/               # Safe Rust wrapper (crates.io)
├── docs/
│   └── research/                # 9 technical research documents
├── meson.build
└── README.md
```

---

## Language Bindings Strategy

### Why We Write in C, Not Rust

libdrmtap is a C library because:
1. **DRM/KMS is inherently C** — `libdrm`, kernel ioctls, `mmap`, `seccomp`, `capset` are all C APIs
2. **Maximum reach** — C is callable from Rust, C++, Python, Go, Zig, Swift, Java (JNI), Node (NAPI)
3. **If we wrote in Rust**, only Rust projects could use us. Sunshine (C++), OBS (C), FFmpeg (C), FreeRDP (C) would be excluded
4. **Rust's `drm-rs` bindings are incomplete** — no `GetFB2`, no modifier support, no `SCM_RIGHTS`
5. **The privilege helper uses Linux-specific syscalls** — `seccomp_rule_add`, `cap_set_proc`, `prctl` — all C

### Rust Bindings (Published)

Three layers, published on crates.io:

```
┌─────────────────────────────────────────────────┐
│  Layer 3: libdrmtap  (safe Rust wrapper)        │
│  • DrmTap::open(), Frame, Display structs       │
│  • Drop trait auto-releases frames              │
│  • Result<T, Error> for all functions            │
│  ✅ Published: crates.io/crates/libdrmtap       │
├─────────────────────────────────────────────────┤
│  Layer 2: libdrmtap-sys  (hand-written FFI)     │
│  • Matches drmtap.h declarations                │
│  • build.rs links to libdrmtap via pkg-config   │
│  ✅ Published: crates.io/crates/libdrmtap-sys   │
├─────────────────────────────────────────────────┤
│  Layer 1: libdrmtap  (C library + .so/.a)       │
│  • Installed via meson install                  │
│  • Provides drmtap.h + libdrmtap.so + .pc       │
└─────────────────────────────────────────────────┘
```

> ⚠️ **Testing status**: Both crates are published and verified to build/test
> on Ubuntu 24.04 (virtio_gpu). Real GPU hardware testing pending.

### Rust API Design

```rust
// libdrmtap-rs — safe Rust wrapper
use libdrmtap::{Context, Config, Frame, Display, Error};

fn main() -> Result<(), Error> {
    // Auto-detect GPU and primary display
    let ctx = Context::open(None)?;
    println!("GPU: {}", ctx.gpu_driver());

    // Enumerate displays
    for display in ctx.list_displays()? {
        println!("  {}: {}x{}@{}Hz",
            display.name, display.width, display.height, display.refresh_hz);
    }

    // Single frame capture
    let frame = ctx.grab_mapped()?;
    save_png(frame.data(), frame.width(), frame.height(), frame.stride());
    // frame auto-released on drop

    // Continuous capture
    while running.load(Ordering::Relaxed) {
        let frame = ctx.grab_mapped()?;
        encoder.send(frame.data(), frame.width(), frame.height());
        // frame auto-released on drop
        std::thread::sleep(Duration::from_micros(16666)); // ~60fps
    }

    Ok(())
}
```

### Why This Pattern Works for RustDesk

RustDesk already depends on 20+ C libraries via FFI:
- `libvpx` (VP8/VP9 codec)
- `libyuv` (pixel format conversion)
- `libsciter` / `libscreencapture` (UI)
- `libopus` (audio codec)
- `libdrm` (DRM access — but they don't use it for capture)

Adding `libdrmtap-sys` is standard practice for them. They `cargo add libdrmtap` and it works.

### Other Language Bindings (Future)

| Language | Binding mechanism | Priority | Target project |
|---|---|---|---|
| **Rust** | `-sys` crate + safe wrapper | 🔴 High | RustDesk |
| **C++** | Direct `#include <drmtap.h>` (C compatible) | ✅ Already works | Sunshine, OBS |
| **Python** | ctypes or cffi | 🟡 Medium | Testing, scripts, CI |
| **Go** | cgo | 🟢 Low | Custom tools |

### Release Status

| Phase | What shipped | Status |
|---|---|---|
| v0.1 | `libdrmtap.so` + `drmtap.h` + `pkg-config` | ✅ Done |
| v0.1 | `libdrmtap-sys` crate (hand-written FFI) | ✅ Published on crates.io |
| v0.1 | `libdrmtap` crate (safe wrapper) | ✅ Published on crates.io |
| v0.1 | EGL/GLES2 GPU-universal detiling backend | ✅ Implemented |
| Next | Hardware testing (Intel, AMD, Nvidia, RPi) | 🔜 Pending hardware access |
| Next | PR to RustDesk with working example | 🔜 After hardware validation |

---

## Thread Safety and Concurrency

### Kernel-Level Concurrency (Between Processes)

The Linux DRM/KMS subsystem handles concurrent access internally:

- **Reference counting** on framebuffer objects — a FB is not freed while anyone holds a reference
- **Per-CRTC locks** — operations on different CRTCs don't block each other
- **Global mutex** only for full modeset operations (resolution changes)
- **Atomic modesetting** — all state changes are applied atomically

Multiple processes can call `drmModeGetFB2()` on the same device concurrently. The kernel serializes access internally. **No user-space locking is needed between processes.**

```
Process A (RustDesk)  ──► drmModeGetFB2() ──┐
                                             ├──► kernel DRM (per-CRTC locks) ──► framebuffer
Process B (NoMachine)  ──► drmModeGetFB2() ──┘
                                                     └── reference counting
```

### Library-Level Thread Safety (Within One Process)

The `drmtap_ctx` struct holds mutable internal state (cached plane IDs, error messages, DMA-BUF references). This state must be protected when shared between threads.

**Strategy**: `pthread_mutex_t` inside `drmtap_ctx`

```c
struct drmtap_ctx {
    int drm_fd;
    uint32_t plane_id;
    char error_msg[256];
    pthread_mutex_t lock;       // protects all mutable state
    // ...
};

int drmtap_grab_mapped(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    pthread_mutex_lock(&ctx->lock);
    // ... capture logic ...
    pthread_mutex_unlock(&ctx->lock);
    return ret;
}
```

**Usage patterns:**

| Scenario | Mutex needed? | Performance | Recommendation |
|---|---|---|---|
| 1 process, 1 ctx, 1 thread | No (but harmless) | Best | Simple use case |
| 1 process, 1 ctx, N threads | Yes (internal) | Good (serialized captures) | Convenience |
| 1 process, N ctx (one per thread) | No | **Best** (zero contention) | **Recommended for high perf** |
| N processes, N ctx | No (kernel handles) | Best | Independent processes |

**Recommendation for users**: For maximum throughput (e.g., multi-monitor capture), create one `drmtap_ctx` per thread. Each context opens its own DRM fd and has independent state — zero lock contention.

### DMA-BUF Frame Ownership

Frames returned by `drmtap_grab()` contain DMA-BUF file descriptors that must be released via `drmtap_frame_release()`. The library uses internal reference counting to track outstanding frames and prevent leaks.

```c
// ✅ CORRECT: release before next grab
drmtap_grab_mapped(ctx, &frame);
process(frame.data);
drmtap_frame_release(ctx, &frame);  // mandatory

// ❌ WRONG: leaking frames
while (1) {
    drmtap_grab_mapped(ctx, &frame);  // fd leak every iteration!
    process(frame.data);
    // forgot drmtap_frame_release()
}
```

---

## Coexistence with Other Software

### Confirmed: Multiple DRM Readers Work Simultaneously

DRM/KMS allows **multiple concurrent readers**. `drmModeGetFB2()` is a read-only query — it does not modify the framebuffer or claim DRM Master. All capture tools are passive observers.

| Combination | Works? | Notes |
|---|---|---|
| libdrmtap + NoMachine | ✅ | Both read, neither is DRM Master |
| libdrmtap + RustDesk (PipeWire) | ✅ | Different mechanisms, no interference |
| libdrmtap + OBS (PipeWire) | ✅ | Different mechanisms |
| libdrmtap + kmsvnc | ✅ | Same DRM mechanism, kernel serializes |
| libdrmtap + Sunshine (KMS) | ✅ | Both read DRM, kernel manages |
| libdrmtap × 2 instances | ✅ | Multiple readers always allowed |

### Why There's No Conflict

```
Compositor (KDE/GNOME) ──► DRM Master ──► WRITES framebuffer
         │
         ├── NoMachine ──► drmModeGetFB2() ──► READS (passive)
         ├── libdrmtap ──► drmModeGetFB2() ──► READS (passive)
         ├── Sunshine  ──► drmModeGetFB2() ──► READS (passive)
         └── kmsvnc    ──► drmModeGetFB2() ──► READS (passive)
```

- The compositor is the **only writer** (DRM Master)
- All capture tools are **readers** — they open the device read-only
- The kernel allows unlimited concurrent readers
- `mmap()` of DMA-BUF uses `PROT_READ` (read-only mapping)

### The Only Restriction: DRM Master

Only **one process** can be DRM Master at a time (normally the compositor). libdrmtap never calls `drmSetMaster()` — it has no need to. We only read.

If someone stops the compositor and starts another one, folibdrmtap will detect the new framebuffer on the next `drmModeGetPlane()` call (because we refresh `fb_id` every frame).

---

## Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Nvidia changes its tiling | Broken capture on Nvidia | Dynamic modifier detection, updatable table |
| Intel CCS can't be disabled | Unreadable buffer | Detect CCS and report clear error to user |
| New kernel breaks API | Build fails | CI with multiple kernel versions, #ifdef guards |
| VAAPI not available | No backend for Intel/AMD | Fallback to dumb buffer + CPU deswizzle |
| RPi 5 restricts GetFB2 | Doesn't work on ARM | Investigate VC6 driver, low priority |
| NixOS/Flatpak paths | Helper not found | Configurable helper path search |
| DMA-BUF fd leak | Memory leak | Reference counting, mandatory frame_release |

---

## Key Findings Summary

### What we learned from each project:

1. **kmsvnc** → How to detect GPU and choose correct pipeline. Manual deswizzle for Nvidia/Intel x-tiled. Driver dispatch structure (`drm_vendors()`).

2. **kms-screenshot** → How to use AMDGPU SDMA as VAAPI alternative. Vulkan compute shader for deswizzle. Complete HDR tone mapping pipeline.

3. **FFmpeg kmsgrab** → How to do robust frame-by-frame capture. GetFB2→GetFB fallback. Multi-plane dedup. Zero-copy output as DRM_PRIME.

4. **gpu-screen-recorder** → Helper binary pattern for permissions. Unix socket + SCM_RIGHTS for passing DMA-BUF fds. No root needed for main app.

5. **RustDesk** → Market validation. 4 years of PipeWire problems. Single dev maintaining Wayland. They need exactly what we're building.

6. **VKMS** → Can test without real hardware in CI. Supports common formats. Limitation: linear only (no tiling).

7. **ReFrame** → GPU-universal detiling via EGL/OpenGL ES. Imports DMA-BUF as `EGLImage` with modifier metadata, GPU handles tiling transparently via `GL_TEXTURE_EXTERNAL_OES`. One code path for ALL GPUs. See [`08_reframe_egl_analysis.md`](08_reframe_egl_analysis.md).
