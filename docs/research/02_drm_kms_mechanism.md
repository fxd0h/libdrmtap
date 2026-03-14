# DRM/KMS Mechanism — Deep Technical Analysis

> **Date**: 2026-03-14  
> **Sources**: kmsvnc/drm.c, kms-screenshot.c, FFmpeg/kmsgrab.c, kernel DRM documentation

---

## DRM/KMS Capture Pipeline

### Flow diagram (what our library must do)

```
1. open("/dev/dri/cardN", O_RDONLY)
       │
2. drmSetClientCap(DRM_CLIENT_CAP_UNIVERSAL_PLANES)
       │
3. drmModeGetPlaneResources() → enumerate planes
       │
4. drmModeGetPlane(plane_id) → find the one with fb_id != 0
       │
5. drmModeGetFB2(fb_id) → get framebuffer info
       │  → width, height, pixel_format, modifier, handles[], pitches[], offsets[]
       │
6. drmPrimeHandleToFD(handle) → export as DMA-BUF fd
       │
7. mmap(prime_fd) → map to userspace memory
       │
8. DMA_BUF_IOCTL_SYNC → synchronize read
       │
9. Read pixels → convert format if needed
```

---

## The 3 Pipelines by GPU

From kmsvnc `drm_vendors()` analysis:

### Pipeline 1: Prime + VAAPI (Intel i915, AMD amdgpu)
```c
// Export framebuffer as DMA-BUF
drmPrimeHandleToFD(drm_fd, fb2->handles[0], O_RDWR, &prime_fd);

// Import into VAAPI for hardware format/tiling conversion
va_init();  // initialize VADisplay on the same DRM device
va_hwframe_to_vaapi(buff);  // GPU converts tiled→linear, formats

// Synchronization
DMA_BUF_IOCTL_SYNC(prime_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
// ... read data ...
DMA_BUF_IOCTL_SYNC(prime_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
```
**Advantage**: GPU handles tiling conversion, zero-copy  
**Dependency**: libva

### Pipeline 2: Dumb Buffer (Nvidia, VMware, VirtualBox, virtio)
```c
// Get global buffer name
struct drm_gem_flink flink = { .handle = fb2->handles[0] };
ioctl(drm_fd, DRM_IOCTL_GEM_FLINK, &flink);

// Open buffer by name
struct drm_gem_open open_arg = { .name = flink.name };
ioctl(drm_fd, DRM_IOCTL_GEM_OPEN, &open_arg);

// Map as dumb buffer
struct drm_mode_map_dumb mreq = { .handle = open_arg.handle };
ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);

// mmap
void *mapped = mmap(NULL, size, PROT_READ, MAP_SHARED, drm_fd, mreq.offset);
```
**Advantage**: Simple, doesn't need VAAPI or Prime  
**Problem**: Doesn't handle tiling well, needs manual deswizzle  
**Note**: Nvidia requires X-TILED deswizzle (16×128)

### Pipeline 3: Prime + direct mmap (simple fallback)
```c
drmPrimeHandleToFD(drm_fd, fb2->handles[0], O_RDWR, &prime_fd);
void *mapped = mmap(NULL, size, PROT_READ, MAP_SHARED, prime_fd, 0);
// + DMA_BUF_IOCTL_SYNC for synchronization
```
**Advantage**: Simplest approach  
**Problem**: Doesn't work with tiled buffers (scrambled result)

---

## The Tiling Problem (Modifiers)

Modern framebuffers are NOT stored in linear memory. Each GPU uses its own tiling format to optimize GPU access.

| GPU Driver | Modifier | Tiling | Solution |
|---|---|---|---|
| Intel i915 | I915_FORMAT_MOD_X_TILED | 128×8 tiles | VAAPI or manual deswizzle |
| Intel i915 | I915_FORMAT_MOD_Y_TILED | 128×8 tiles | VAAPI |
| Intel CCS | I915_FORMAT_MOD_CCS | Compressed | ❌ Doesn't work, requires `INTEL_DEBUG=noccs` |
| AMD amdgpu | AMD_FMT_MOD_* | Variable | VAAPI or AMDGPU SDMA copy |
| Nvidia | Custom | 16×128 tiles | Manual CPU deswizzle |
| VM (vmwgfx, virtio) | DRM_FORMAT_MOD_LINEAR | Linear ✅ | Direct, no conversion |

**Key for libdrmtap**: detect the modifier and choose the correct pipeline automatically.

### Manual Deswizzle (kmsvnc)
```c
// For generic X-TILED tiling
for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
        int sno = (x / tilex) + (y / tiley) * (width / tilex);
        int ord = (x % tilex) + (y % tiley) * tilex;
        int offset = sno * tilex * tiley + ord;
        memcpy(output + (x + y * width) * 4, input + offset * 4, 4);
    }
}
```

---

## Pixel Formats Found

| fourcc | Bits/px | Description | Frequency |
|---|---|---|---|
| XRGB8888 | 32 | RGB, alpha ignored | Very common |
| ARGB8888 | 32 | RGB with alpha | Common |
| XBGR8888 | 32 | BGR, alpha ignored | AMD |
| ABGR8888 | 32 | BGR with alpha | AMD |
| RGB565 | 16 | Compact RGB | Embedded |
| ABGR16161616 | 64 | HDR 16-bit/channel | HDR displays |
| XR30 / AR30 | 30 | 10-bit per channel | HDR |

---

## CRTC and Correct Plane Detection

kmsvnc implements this logic:

1. Enable universal planes: `drmSetClientCap(DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)`
2. Enumerate all planes: `drmModeGetPlaneResources()`
3. For each plane, get type via properties:
   - `DRM_PLANE_TYPE_PRIMARY` → main framebuffer (what we want)
   - `DRM_PLANE_TYPE_CURSOR` → cursor (optional)
   - `DRM_PLANE_TYPE_OVERLAY` → overlays (ignore)
4. Filter by CRTC if one is specified
5. Verify that `plane->fb_id != 0` (has active framebuffer)

**For multi-monitor**: each monitor has its own CRTC → its own primary plane → its own framebuffer.

---

## Required Permissions

| Operation | Permission | Alternative |
|---|---|---|
| `open("/dev/dri/card0")` | Root or `video` group | udev rules |
| `drmModeGetFB2()` | `DRM_CAP_DUMB_BUFFER` + `CAP_SYS_ADMIN` | — |
| `drmPrimeHandleToFD()` | Handle access | — |
| `DRM_IOCTL_GEM_FLINK` | Master or `CAP_SYS_ADMIN` | — |

**kms-screenshot**: requires `sudo` directly  
**kmsvnc**: opens `/dev/dri/cardN` as O_RDONLY, drops master when not needed  
**gpu-screen-recorder**: uses `kms_server` helper with `setcap cap_sys_admin+ep`

---

## Key Findings for libdrmtap

### 1. We need VAAPI as the primary backend
kmsvnc and gpu-screen-recorder use it for Intel and AMD. It's what solves tiling without CPU overhead. Without VAAPI, we need manual deswizzle which is slow.

### 2. drmModeGetFB2 is mandatory (not GetFB)
The old `drmModeGetFB()` API doesn't return pixel format or modifier. Without that info we can't decode the buffer. `drmModeGetFB2()` requires kernel ≥ 4.11.

### 3. DMA_BUF_IOCTL_SYNC is essential for continuous capture
Without sync, concurrent reads with the GPU produce tearing or corrupted data.

### 4. Intel CCS is a documented special case
kmsvnc explicitly documents it: "please set INTEL_DEBUG=noccs". It's a gotcha our library must detect and report to the user.

### 5. The privileged helper pattern is the industry standard
gpu-screen-recorder uses a separate helper process (`kms_server`) with `cap_sys_admin`. This allows the main app to run without privileges. Our library should follow this pattern.

### 6. kms-screenshot shows the alternative AMDGPU approach
Uses SDMA copy (AMD's DMA engine) to copy from tiled framebuffer to a linear buffer, without VAAPI. More complex but doesn't require libva. Also has Vulkan as an alternative.
