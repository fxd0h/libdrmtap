# DRM/KMS Mechanism — Deep Technical Analysis

> **Date**: 2026-03-14 (research snapshot; the **Key Findings** section has been reconciled with the shipped design)  
> **Sources**: kmsvnc/drm.c, kms-screenshot.c, FFmpeg/kmsgrab.c, kernel DRM documentation

> **Reading note**: the pipelines below survey how *other* tools tackle DRM/KMS capture. What
> libdrmtap actually built — a GPU-universal **EGL/GLES2** detile backend plus a zero-copy
> **DMA-BUF + `SCM_RIGHTS` (V3)** helper protocol — is summarised in **Key Findings for libdrmtap**.

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

**Key for libdrmtap**: detect the modifier and route accordingly — linear buffers are mapped
directly, and everything tiled/compressed (Intel X/Y + CCS, AMD, Nvidia block-linear) goes through
the GPU-universal **EGL** detile path. No per-vendor VAAPI pipeline is required (see Key Findings #1).

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

> ⚠️ **HDR is not properly handled yet.** Today the AR30/XR30 (10-bit) path is a naïve bit-shift
> that keeps the top 8 of 10 bits — no PQ decode, no BT.2020 primaries, no tone-mapping — so an HDR
> scanout comes out as a washed-out, truncated SDR frame. ABGR16161616 (16-bit) and 10-bit YUV (P010)
> are not handled, and `HDR_OUTPUT_METADATA` / connector `Colorspace` are not read. Proper HDR10 is an
> open item ([#16](https://github.com/fxd0h/libdrmtap/issues/16)) and the current top blocker — do not
> treat any 10-bit/HDR row above as "supported".

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

### 1. A GPU-universal EGL/GLES2 detile backend, not VAAPI
kmsvnc and gpu-screen-recorder reach for VAAPI on Intel and AMD, but VAAPI is vendor-specific and
pulls in libva. libdrmtap instead uses a single EGL/OpenGL ES backend (`src/gpu_egl.c`): import the
DMA-BUF as an `EGLImage` (carrying the DRM format modifier), draw it, and `glReadPixels` into a linear
RGBA buffer. Because `EGL_EXT_image_dma_buf_import` delegates the de-tiling to the driver, one code
path covers Intel X/Y-tiled + CCS, AMD, and Nvidia block-linear. EGL is the **primary** detile path; a
CPU deswizzle survives only as a fallback for a few formats. Plain linear framebuffers (virtio-gpu,
simple VMs) are mapped directly with no detile at all.

### 2. drmModeGetFB2 is mandatory (not GetFB)
The old `drmModeGetFB()` API doesn't return pixel format or modifier. Without that info we can't decode the buffer. `drmModeGetFB2()` requires kernel ≥ 4.11.

### 3. DMA_BUF_IOCTL_SYNC is essential for continuous capture
Without sync, concurrent reads with the GPU produce tearing or corrupted data.

### 4. Intel CCS is handled by the EGL path, not a dead end
kmsvnc punts on CCS ("please set `INTEL_DEBUG=noccs`"). Because the EGL importer hands the DRM modifier
straight to the driver, libdrmtap de-tiles CCS framebuffers transparently — verified on dual-4K Meteor
Lake (i915) over the V3 zero-copy path. So CCS is a handled case, not a gotcha we have to report back to
the user.

### 5. The privileged helper pattern is the industry standard
gpu-screen-recorder uses a separate helper process (`kms_server`) with `cap_sys_admin`. This allows the main app to run without privileges. Our library should follow this pattern.

### 6. kms-screenshot shows the alternative AMDGPU approach
Uses SDMA copy (AMD's DMA engine) to copy from tiled framebuffer to a linear buffer, without VAAPI. More complex but doesn't require libva. Also has Vulkan as an alternative.

### 7. Zero-copy via DMA-BUF + SCM_RIGHTS (the V3 path)
The privileged helper does not stream pixels over the socket. It exports the scanout as a DMA-BUF and
passes the **fd** to the unprivileged library via `SCM_RIGHTS` (the "V3" protocol); the library then
maps or EGL-imports it directly, so no full frame ever crosses the socket. A **V2** fallback (dumb-map
the framebuffer and copy the bytes over the socket) is kept for cases where DMA-BUF export is not
possible. On plain virtio the V3 path costs ≈1.2% of one core total (helper ≈0.7%), versus ≈14% on the
old V2 copy path.

### 8. virgl (host-rendered virtio 3D) needs GPU readback, not TRANSFER_FROM_HOST
A virgl scanout is rendered on the **host** GPU; a guest CPU `mmap` of the DMA-BUF comes back black, and
`VIRTGPU_TRANSFER_FROM_HOST` does **not** bring those pixels back into guest memory either. The working
answer is the same EGL backend: import the buffer on the guest GPU and `glReadPixels`, which reads the
host-rendered pixels correctly. This is solved technically; productionizing it is tracked in
[#15](https://github.com/fxd0h/libdrmtap/issues/15). Plain (non-virgl) virtio is unaffected: there the
host-rendered pixels do land in guest RAM, so the library's direct path pulls them with
`TRANSFER_FROM_HOST` + a dumb map, while the helper path serves the same scanout zero-copy over V3. It is
specifically the virgl 3D scanout that `TRANSFER_FROM_HOST` cannot recover.
