# ReFrame Analysis — EGL/OpenGL GPU-Universal Detiling

> **Date**: 2026-03-14  
> **Source**: [AlynxZhou/reframe](https://github.com/AlynxZhou/reframe) — 36 releases, C++, LGPL-3.0  
> **Relevance**: Proves that a single EGL path handles ALL GPU tiling formats

---

## Architecture Overview

ReFrame is a DRM/KMS-based VNC server with a split-process design:

```
reframe-streamer (privileged, CAP_SYS_ADMIN)
    ├── Opens /dev/dri/cardN
    ├── drmModeGetFB2() → export DMA-BUF fds + modifier
    ├── uinput keyboard/mouse injection
    └── Sends DMA-BUF fds via UNIX socket (SCM_RIGHTS)

reframe-server (unprivileged)
    ├── Receives DMA-BUF fds
    ├── EGL import → GL_TEXTURE_EXTERNAL_OES → render to linear RGBA
    ├── glReadPixels() → linear pixel data
    ├── Damage detection (CPU or GPU)
    └── VNC encoding + server
```

This is very similar to our helper + library architecture.

---

## Key Finding: EGL Handles ALL GPU Tiling

ReFrame does **NOT** have per-GPU detiling code. Instead:

1. Streamer sends: DMA-BUF fd + fourcc + modifier + pitches + offsets
2. Server calls `eglCreateImage(EGL_LINUX_DMA_BUF_EXT)` with modifier
3. Binds as `GL_TEXTURE_EXTERNAL_OES` and renders to FBO
4. `glReadPixels()` gives linear RGBA — GPU driver handled the tiling

### Why GL_TEXTURE_EXTERNAL_OES?

From `rf-converter.c` line 901-904:
```c
// While GL_TEXTURE_2D does work for most cases, it won't work with
// NVIDIA and linear modifier (which is used by TTY), we have to use
// GL_TEXTURE_EXTERNAL_OES.
```

This is the critical detail: `GL_TEXTURE_EXTERNAL_OES` + `samplerExternalOES`
delegates format interpretation entirely to the driver.

---

## EGL Import Code (simplified from rf-converter.c)

```c
EGLAttrib attribs[] = {
    EGL_WIDTH,                 width,
    EGL_HEIGHT,                height,
    EGL_LINUX_DRM_FOURCC_EXT,  fourcc,
    EGL_DMA_BUF_PLANE0_FD_EXT,       fd,
    EGL_DMA_BUF_PLANE0_OFFSET_EXT,   offset,
    EGL_DMA_BUF_PLANE0_PITCH_EXT,    pitch,
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (modifier & 0xFFFFFFFF),
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (modifier >> 32),
    EGL_NONE
};

EGLImage image = eglCreateImage(display, EGL_NO_CONTEXT,
                                EGL_LINUX_DMA_BUF_EXT, NULL, attribs);

// Bind as external texture — driver handles tiling transparently
glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

// Render to FBO, then glReadPixels() → linear RGBA
```

---

## Comparison: EGL Path vs Per-GPU CPU Deswizzle

| | EGL path (ReFrame) | CPU deswizzle (our current backends) |
|---|---|---|
| **Code** | ~200 lines, universal | ~100 lines per GPU × 4 GPUs |
| **GPU support** | Intel, AMD, Nvidia, RPi, any EGL | Each GPU manually coded |
| **CCS/DCC compressed** | ✅ Handled by driver | ❌ Cannot read without VAAPI |
| **Performance** | GPU-accelerated | CPU-bound |
| **Dependencies** | libEGL, libGLESv2 | None |
| **Headless/no-GPU** | ❌ Needs working EGL | ✅ Works with dumb mmap |
| **Tested in prod** | Yes, 36 releases | Not tested on real hardware |

---

## Strategy for libdrmtap

### Recommended approach: dual path

```
drmtap_grab_mapped()
    ├── modifier == LINEAR → gpu_generic.c (mmap, zero deps)
    ├── EGL available → gpu_egl.c (universal, GPU-accelerated)
    └── EGL unavailable → gpu_{intel,amd,nvidia}.c (CPU fallback)
```

### Dependencies for EGL path

```
# Build: pkg-config
egl >= 1.4
glesv2 >= 2.0

# Runtime: drivers
mesa-libEGL or nvidia-egl
mesa-libGLESv2 or nvidia-glesv2
```

### What gpu_egl.c needs

1. `eglQueryDevicesEXT()` — find EGL device for our DRM card
2. `eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT)` — headless EGL display
3. `eglCreateImage(EGL_LINUX_DMA_BUF_EXT)` — import DMA-BUF with modifier
4. `glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES)` — bind as texture
5. Render fullscreen quad → FBO → `glReadPixels()` → linear RGBA

---

## Bonus Findings from ReFrame

- **uinput**: Full keyboard + mouse injection via `/dev/uinput` (lines 704-761)
- **Screen wakeup**: Moves mouse 1px to wake screen before capture (line 680-702)
- **Damage detection**: Only sends changed tiles (CPU or GPU comparison)
- **Rotation**: Supports screen rotation via MVP matrix in shader
- **Double-buffer**: Gets fb_id each frame to handle compositor double-buffering
- **drmDropMaster()**: Drops DRM master so compositor can start after ReFrame
