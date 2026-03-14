# GPU Differences and Testing Strategy

> **Date**: 2026-03-14  
> **Sources**: kmsvnc/drm.c, kms-screenshot.c, kernel.org vkms, DRM-CI

---

## GPU Driver Differences

### Intel i915 / xe

| Aspect | Detail |
|---|---|
| Pipeline | Prime → VAAPI (hardware convert) |
| Tiling | X-TILED (128×8), Y-TILED, CCS (compressed) |
| Common format | XRGB8888, ARGB8888, XR30 (30-bit) |
| Gotcha #1 | **CCS (Color Control Surface)**: CCS framebuffers are not readable. Requires `INTEL_DEBUG=noccs` globally |
| Gotcha #2 | Newer drivers (`xe`) may have different APIs |
| VAAPI | ✅ Excellent support, iHD/i965 drivers |
| Ref code | kmsvnc: `drm_kmsbuf_prime_vaapi()` |

### AMD amdgpu

| Aspect | Detail |
|---|---|
| Pipeline | Prime → VAAPI **or** AMDGPU SDMA copy |
| Tiling | AMD_FMT_MOD_* (variable, device-specific) |
| Common format | XRGB8888, ABGR8888, ABGR16161616 (HDR) |
| Gotcha | HDR (16-bit/channel) requires tone mapping to convert to 8-bit |
| VAAPI | ✅ Good support via mesa radeonsi |
| SDMA | Dedicated DMA engine for GPU→CPU copy |
| Vulkan | Alternative for deswizzle via compute shader |
| Ref code | kms-screenshot: `capture_framebuffer_amdgpu()`, `amdgpu_copy_buffer()` |

### Nvidia nvidia-drm

| Aspect | Detail |
|---|---|
| Pipeline | Dumb buffer → CPU deswizzle (X-TILED 16×128) |
| Status | ⚠️ "Highly experimental" in kmsvnc |
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
| Gotcha | virgl (virtio 3D) does NOT work with dumb path |
| Ref code | kmsvnc: generic path |

### Embedded (Raspberry Pi, Jetson)

| Aspect | Detail |
|---|---|
| Status | Under-researched |
| RPi 5 (VC6) | ⚠️ May restrict framebuffer access via GetFB2 |
| Jetson | Uses own DRM driver, needs specific research |
| Priority | Low for v1, focus on desktop first |

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
- LattePanda/NUC with Intel i915 → VAAPI path
- PC with AMD discrete → AMDGPU path
- VM with virtio-gpu → dumb buffer path
- Nvidia → deswizzle path (lowest priority)

#### Proposed CI Pipeline
```yaml
# .gitlab-ci.yml or GitHub Actions
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
