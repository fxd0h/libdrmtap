# GPU Differences and Testing Strategy

> **Date**: 2026-03-14  
> **Sources**: kmsvnc/drm.c, kms-screenshot.c, kernel.org vkms, DRM-CI

> **Update (libdrmtap 0.4.3)** — The tables below capture the early *research* into how reference projects (kmsvnc, kms-screenshot, FFmpeg) deswizzle each vendor. libdrmtap ultimately took a different route: a single GPU-universal **EGL/GLES2 detiling** backend (`src/gpu_egl.c`) imports the scanout DMA-BUF as an `EGLImage`, draws it, and `glReadPixels()` to linear RGBA — so per-vendor tiling (Intel X/Y-tiled + CCS, AMD modifiers, Nvidia block-linear) is handled by the driver itself, with no per-GPU CPU deswizzle in the common case. A CPU deswizzle remains only as a fallback for some formats. **Verified:** Intel `i915`/`xe` (dual-4K Meteor Lake, CCS), Nvidia `nvidia-drm` incl. Tegra/Jetson (Orin Nano, aarch64), and virtio-gpu. **AMD `amdgpu` is implemented but not yet verified on real hardware.**
>
> ⚠️ **HDR is _not_ supported yet** — it is the current top blocker, tracked in [#16](https://github.com/fxd0h/libdrmtap/issues/16). The AR30/XR30 (10-bit) path is a naive bit-shift that keeps the top 8 of 10 bits (no PQ decode, no BT.2020, no tone-mapping), the EGL path outputs 8-bit, and 16-bit (`XR48`/`AR48`) and 10-bit YUV (`P010`) are not handled. Capturing an HDR scanout today yields a truncated, washed-out SDR result.

---

## GPU Driver Differences

### Intel i915 / xe

| Aspect | Detail |
|---|---|
| Pipeline | Prime → VAAPI (hardware convert) |
| Tiling | X-TILED (128×8), Y-TILED, CCS (compressed) |
| Common format | XRGB8888, ARGB8888, XR30 (30-bit) |
| Gotcha #1 | **CCS (Color Control Surface)**: opaque to CPU mmap; the `INTEL_DEBUG=noccs` workaround disables compression globally. libdrmtap avoids that by reading CCS framebuffers through the EGL detile path instead (✅ verified on Meteor Lake) |
| Gotcha #2 | Newer drivers (`xe`) may have different APIs |
| VAAPI | ✅ Excellent support, iHD/i965 drivers |
| Ref code | kmsvnc: `drm_kmsbuf_prime_vaapi()` |

### AMD amdgpu

| Aspect | Detail |
|---|---|
| Pipeline | Prime → VAAPI **or** AMDGPU SDMA copy |
| Tiling | AMD_FMT_MOD_* (variable, device-specific) |
| Common format | XRGB8888, ABGR8888, ABGR16161616 (HDR) |
| Gotcha | HDR (16-bit/channel, `ABGR16161616`) needs PQ decode + tone-mapping to land in 8-bit — **not done in libdrmtap yet** ([#16](https://github.com/fxd0h/libdrmtap/issues/16)) |
| VAAPI | ✅ Good support via mesa radeonsi |
| SDMA | Dedicated DMA engine for GPU→CPU copy |
| Vulkan | Alternative for deswizzle via compute shader |
| Ref code | kms-screenshot: `capture_framebuffer_amdgpu()`, `amdgpu_copy_buffer()` |

### Nvidia nvidia-drm

| Aspect | Detail |
|---|---|
| Pipeline | Dumb buffer → CPU deswizzle (X-TILED 16×128) in kmsvnc; libdrmtap uses the EGL detile path instead |
| Status | ⚠️ "Highly experimental" in kmsvnc — but libdrmtap captures Nvidia incl. Tegra/Jetson via EGL (✅ verified on Orin Nano, aarch64, Wayland scanout, V3) |
| Requirement | nvidia-open or nvidia-legacy with DRM enabled |
| VAAPI | ❌ Not natively supported (needs nvidia-vaapi-driver, buggy) |
| Gotcha #1 | Only one modifier (X-TILED) currently supported |
| Gotcha #2 | Requires `DRM_IOCTL_GEM_FLINK` + `DRM_IOCTL_MODE_MAP_DUMB` |
| Deswizzle | Manual CPU: 16×128 pixel tiles |
| Ref code | kmsvnc: `convert_nvidia_x_tiled_kmsbuf()` |

### VMs (vmwgfx, vboxvideo, virtio_gpu)

| Aspect | Detail |
|---|---|
| Pipeline | Direct dumb buffer (linear) |
| Tiling | DRM_FORMAT_MOD_LINEAR ✅ no conversion needed |
| Format | XRGB8888 |
| Gotcha | **virgl** (host-rendered virtio 3D) is invisible to the dumb/CPU path — the guest reads back black, and `TRANSFER_FROM_HOST` does not bring it back. libdrmtap captures it via GPU-side EGL `glReadPixels()` on the guest GPU (solved technically; production integration tracked in [#15](https://github.com/fxd0h/libdrmtap/issues/15)). Plain (non-virgl) virtio works via direct linear map + V3 zero-copy ✅ |
| Ref code | kmsvnc: generic path |

### Embedded (Raspberry Pi, Jetson)

| Aspect | Detail |
|---|---|
| Status | Under-researched |
| RPi 5 (VC6) | ⚠️ May restrict framebuffer access via GetFB2 |
| Jetson | ✅ Verified on Orin Nano (Tegra `nvidia-drm`, aarch64) — EGL detile, V3, Wayland scanout |
| Priority | RPi still under-researched; Jetson/Tegra done |

---

## Driver Support Matrix (kmsvnc)

```c
// From drm_vendors() in kmsvnc/drm.c:

if (i915 || amdgpu)     → Prime + VAAPI
if (nvidia-drm)          → Dumb + X-TILED deswizzle CPU
if (vmwgfx/vbox/virtio) → Direct dumb
else                     → Dumb (warning: untested)
```

---

## Testing Strategy with VKMS

### What is VKMS?

**Virtual Kernel Mode Setting** — kernel driver that simulates a GPU without real hardware.

```bash
# Load the module
sudo modprobe vkms

# Appears as
ls -la /dev/dri/card*    # → /dev/dri/cardN (new)
```

### VKMS Capabilities (2024-2025)

| Feature | Status |
|---|---|
| Formats: ARGB8888, XRGB8888, RGB565 | ✅ |
| Formats: ARGB16161616, XRGB16161616 | ✅ |
| Alpha blending | ✅ Full |
| Overlay planes (up to 8) | ✅ |
| Plane rotation | ✅ |
| Multiple CRTCs | 🔄 In development |
| Modifiers/Tiling | ❌ LINEAR only |
| DMA-BUF export | ✅ |
| Writeback connector | ✅ |

### Testing Plan for libdrmtap

#### Level 1: Unit tests with VKMS (CI, no hardware)
```bash
# Setup
sudo modprobe vkms
export DRMTAP_TEST_CARD=/dev/dri/card1  # vkms device

# Possible tests:
# ✅ Plane/CRTC enumeration
# ✅ drmModeGetFB2 with supported formats
# ✅ drmPrimeHandleToFD
# ✅ mmap of dumb buffers
# ✅ Format conversion XRGB→RGB
# ✅ Multi-plane enumeration
# ❌ Tiling/deswizzle (vkms is linear only)
# ❌ VAAPI (no hardware)
```

#### Level 2: Integration with igt-gpu-tools
```bash
# igt-gpu-tools is the official DRM testing suite
# https://gitlab.freedesktop.org/drm/igt-gpu-tools
# Can run on vkms

igt_runner --device /dev/dri/card1 tests/kms_*
```

#### Level 3: Real hardware testing
- Intel `i915`/`xe` (Meteor Lake) → EGL detile path — ✅ verified (dual 4K, CCS)
- AMD `amdgpu` → EGL detile / CPU fallback — implemented, **not yet hardware-verified**
- VM with virtio-gpu → direct linear map + V3 zero-copy — ✅ verified
- Nvidia incl. Tegra/Jetson → EGL detile path — ✅ verified (Orin Nano, aarch64)

#### CI Pipeline (now live)

GitHub Actions runs **Build & Test on Ubuntu 22.04 and 24.04**, a **Rust-crate** job, **cppcheck** static analysis, **CodeQL**, and **CodeRabbit** review on every PR; ASan/UBSan + cppcheck are part of local validation. The sketch below captures the core flow:

```yaml
# GitHub Actions (core flow)
test:
  script:
    - sudo modprobe vkms
    - meson setup build
    - meson compile -C build
    - meson test -C build --suite unit
    # Integration tests require vkms
    - DRM_DEVICE=/dev/dri/card1 meson test -C build --suite integration
```

---

## FFmpeg kmsgrab — Reference Implementation Analysis

### Structure (KMSGrabContext)
```c
typedef struct KMSGrabContext {
    AVBufferRef *device_ref;     // DRM device
    AVDRMDeviceContext *hwctx;   // device fd
    int fb2_available;           // supports GetFB2?
    
    uint32_t plane_id;           // selected plane
    uint32_t drm_format;         // fourcc
    unsigned int width, height;
    
    int64_t frame_delay;         // framerate control
    int64_t drm_format_modifier; // tiling modifier
    int64_t source_plane;        // plane override
    int64_t source_crtc;         // CRTC override
    AVRational framerate;        // capture framerate
} KMSGrabContext;
```

### Per-frame capture pattern
```c
// Each frame:
1. drmModeGetPlane(plane_id)          // refresh plane state
2. Verify plane->fb_id != 0           // still has framebuffer?
3. drmModeGetFB2(fb_id)               // get current FB info
4. For each handle: drmPrimeHandleToFD()  // export as DMA-BUF
5. Fill AVDRMFrameDescriptor           // fd + size + format + pitch
6. Build AVFrame with DRM_PRIME        // zero-copy to pipeline
```

### Lessons from FFmpeg code

1. **Fallback GetFB2 → GetFB**: If `drmModeGetFB2` returns ENOSYS, falls back to old `drmModeGetFB`
2. **Change detection**: Each frame verifies that width/height/format haven't changed
3. **Multi-plane formats**: Handles up to 4 handles, deduplicating when they're the same object
4. **O_RDONLY**: DMA-BUFs are exported read-only (not O_RDWR like kmsvnc)
5. **Configurable options**: device path, format, modifier, CRTC, plane, framerate
6. **Output format**: `AV_PIX_FMT_DRM_PRIME` (zero-copy DMA-BUF descriptor, not RGB)
