# Deep Analysis of PRs, Issues, and GitHub Discussions

> **Date**: 2026-03-14  
> **Sources**: GitHub issues and PRs from kmsvnc (25 issues), gpu-screen-recorder, kms-screenshot, RustDesk, FFmpeg

---

## kmsvnc — Complete Autopsy (25 issues, 3 PRs)

### Most frequent bug: Unsupported pixel formats

| Issue | Format | GPU | Status | Impact |
|---|---|---|---|---|
| #1 | AR30 (10-bit) | AMD 6900XT | ⛔ Open since May 2023 | KDE Wayland uses 30-bit by default |
| #22 | AB48 (16-bit HDR) | AMD RX 550 | ⛔ Open | Fedora 42, kernel 6.14 |
| #24 | AB30 (10-bit) | Nvidia GTX | ⛔ Open | Nvidia BLOCK_LINEAR tiling |
| #7 | XRGB misdetection | AMD | 🔧 Resolved | Blue tint, incorrect channel swap |

**Lesson for libdrmtap**: Modern compositors (KDE, GNOME with HDR) output 10-bit (AR30/XR30) and 16-bit (AB48) by default. Supporting only XRGB8888 is insufficient for 2024+. User tried `KWIN_DRM_PREFER_COLOR_DEPTH=24` to force 8-bit but it didn't work.

### Bug #10: Double-buffering frame skip (Weston)

> "weston uses two framebuffer IDs and flips between them for each frame. kmsvnc will use the currently active framebuffer ID. You will see the screen updates every 2 key presses instead of every key press."

**Root cause**: kmsvnc captures the framebuffer that was active at startup. Weston does page-flipping (alternates between 2 FBs). When the compositor switches to the other FB, kmsvnc keeps reading the old one.

**Lesson for libdrmtap**: Implement `drmModeGetPlane()` on **every frame** (like FFmpeg kmsgrab) to detect `fb_id` changes. Never cache the framebuffer ID.

### Bug #17: Cursor X position off by 2x (multi-monitor)

> "moving the mouse to the far right of the VNC client window puts the cursor on the right side of the right-hand monitor (crtc 80) instead of the right side of the left-hand monitor (crtc 77)"

**Root cause**: With 2 monitors at 1920px, the CRTC reports coordinates in the combined space (0-3840), but the cursor plane only has 0-1920. The conversion doesn't adjust.

**Lesson for libdrmtap**: Cursor coordinate space is relative to CRTC, not plane. In multi-monitor setups, mapping must be done correctly.

### Bug #19: vkms doesn't work ("DRM ioctl error -1")

> "I am trying to get this to work on a vkms device. [...] DRM ioctl error -1 on line 669"

**Root cause**: vkms doesn't support `DRM_IOCTL_GEM_FLINK` (the ioctl kmsvnc uses for "dumb buffer" path). The dumb buffer path fails with vkms.

**Solution**: Use `drmPrimeHandleToFD` instead of GEM_FLINK for vkms. The issue also confirms that on ArchLinux+Nvidia, vkms shows no active planes.

**Lesson for libdrmtap**: For vkms testing, use Prime pipeline (not dumb). Also, vkms needs something rendering to it (compositor) to have active planes.

### Bug #18: "No handle set on framebuffer" (CAP_SYS_ADMIN)

> "handles 0 0 0 0" without root, "handles 1 0 0 0" with `setcap cap_sys_admin=ep`

**Technical details from log**:
- Driver: amdgpu  
- Modifier: `AMD:GFX10_RBPLUS,GFX9_64K_R_X,DCC,DCC_RETILE,DCC_INDEPENDENT_128B,DCC_MAX_COMPRESSED_BLOCK=128B,DCC_CONSTANT_ENCODE,PIPE_XOR_BITS=4,PACKERS=3`
- Without CAP_SYS_ADMIN: `handles 0 0 0 0` → kernel hides the handles
- With CAP_SYS_ADMIN: `handles 1 0 0 0` → enabled

**Lesson for libdrmtap**: Confirms our API must detect `handles[0] == 0` and activate the helper automatically.

### Bug #25: iMX8 ARM — 512-byte offset + multiple framebuffers

> "I noticed that in one buffer display data are offset by 512 bytes"

An ARM developer (iMX8) submitted 3 patches:
1. Multiple framebuffer support (double-buffering)
2. Fix for starting data address (offset)
3. `imx-dcss` driver as new backend

**Lesson for libdrmtap**: ARM SoCs have arbitrary offsets in framebuffers. Direct mapping `mmap(fd, 0)` doesn't always start at byte 0.

### Bug #21: Intel Atom VAAPI fails

> "va operation error 0x14 the requested function is not implemented"

Intel CherryView (Atom x5-Z8330) with the old i965 driver fails on `vaGetImage`. Old VAAPI doesn't support the operation.

**Lesson for libdrmtap**: Fallback needed when VAAPI reports "not implemented". Don't assume Intel always = functional VAAPI.

### Bug #20: Intel JasperLake — "No usable planes found"

All planes have `CRTC 0 FB 0`. Monitor is connected but compositor isn't using that card/CRTC.

**Lesson for libdrmtap**: There can be multiple DRM devices. If all planes on a device are empty, try another `/dev/dri/cardN`.

### Bug #9: Raspberry Pi — Broadcom T-TILED

> "Support Raspberrypi VC4 Broadcom T tiled"

Mentions an existing project: `drm-vc4-grabber` (Rust) that takes screenshots on RPi by decoding Broadcom's T-tiling.

**Lesson for libdrmtap**: RPi needs its own deswizzle (T-tiled, different from Intel X-tiled or Nvidia).

### Bug #16: Panel rotation

> "The captured image may require rotation according to panel orientation property of the DRM connector"

References kernel documentation on `panel orientation` property.

**Lesson for libdrmtap**: Tablets and some laptops have rotated panels (90°, 180°, 270°). The framebuffer is captured "sideways". Must read the connector's orientation property.

### Bug #14: OpenGL apps lose capture

> "going from tty <-> games frontend <-> game/emulator, the video is always lost on the <->. I suspect this happens because framebuffers are destroyed and new ones are created"

**Root cause**: DRM context switch. When a fullscreen OpenGL app creates new framebuffers, the previous FB_ID is destroyed. kmsvnc loses the reference.

**Lesson for libdrmtap**: Implement automatic reconnection when framebuffer changes. Detect new fb_id on each frame (like FFmpeg).

### Bug #8: API compat — old kernel

> "`SYS_pidfd_getfd` syscall available since Linux 5.6" + "`fourcc_mod_is_vendor` macro added 11 months ago, even ubuntu22.04 can not support"

Ubuntu 20.04 (kernel 5.4) doesn't have `pidfd_getfd` or new libdrm macros.

**Lesson for libdrmtap**: Define minimum supported kernel. Provide `#ifndef` fallbacks for libdrm macros that don't exist in older versions.

### Issue #11: "Consider using neatvnc instead of libvncserver"

Weston already uses neatvnc. Suggests more permissive license (MIT vs GPL).

**Lesson for libdrmtap**: MIT/BSD license for maximum adoption. GPL is a problem for RustDesk (Apache 2.0) and proprietary projects.

---

## gpu-screen-recorder — gsr-kms-server Findings

### Confirmed IPC architecture
- `gsr_kms_client_init` creates socketpair (not named Unix socket)
- They replace file-backed socket with socketpair for security
- DMA-BUF FDs are passed via SCM_RIGHTS in socket messages

### Permission problems in production
1. **NixOS**: Binary paths not resolvable for `setcap` due to Nix store
2. **Flatpak**: Helper can't be found from the sandbox
3. **Polkit**: Repetitive popup every time recording starts

### DMA-BUF error on Nvidia+Wayland
> "Unable to map DMABUF exported memory to CPU visible buffer: Permission denied"

Nvidia on Wayland has additional restrictions for mmapping exported DMA-BUFs.

---

## Recurring Problem Patterns (cross-project)

### 1. Formats and color depth
```
Affected projects: kmsvnc, kms-screenshot
Frequency: ~30% of all issues
Problematic formats: AR30, AB30, AB48, ABGR16161616
Cause: HDR/10-bit compositors by default in 2024+
```

### 2. Permissions and capabilities
```
Affected projects: kmsvnc, gpu-screen-recorder, FFmpeg
Frequency: ~20% of issues
Cause: kernel 5.x+ hides handles without CAP_SYS_ADMIN
Standard solution: setcap cap_sys_admin+ep on helper
```

### 3. Tiling/deswizzle
```
Affected projects: all
Frequency: ~15% of issues
Variants: Intel X-TILED, Intel CCS, Nvidia BLOCK_LINEAR, AMD DCC, Broadcom T-TILED
No universal solution: each GPU needs its own code path
```

### 4. CCS-compressed framebuffers crash CPU deswizzle (discovered 2026-03-18)
```
Affected projects: libdrmtap (our own!)
Frequency: 100% on Intel Gen12+ with helper mode
GPU: Intel UHD (TGL/ADL) with I915_FORMAT_MOD_Y_TILED_CCS
Cause: dumb_mmap of CCS framebuffers returns compressed data that
       cannot be CPU-deswizzled. Only EGL import via DMA-BUF fd works.
Impact: Silent process crash (out-of-bounds read in deswizzle)
```

**Root cause chain:**
1. Helper does `MODE_MAP_DUMB` + `mmap` → gets CCS-compressed pixels
2. Sends raw CCS data over socket with `modifier = CCS`
3. Parent has `dma_buf_fd = -1` (V2 protocol: no SCM_RIGHTS) → EGL skipped
4. CPU `deswizzle_intel_y_tiled()` runs on CCS data → out-of-bounds → crash

**Why it wasn't caught before:**
- vkms (test VM): modifier = LINEAR → deswizzle skipped entirely → no crash
- Standalone test (root): has `dma_buf_fd >= 0` → EGL GPU deswizzle → correct image
- Only triggers when: Intel CCS + helper (non-root) + V2 protocol

**Fix applied:** CCS modifiers (0x05-0x08) return `-ENOTSUP` from `drmtap_deswizzle()`.
`gpu_auto_process()` handles this by setting modifier to LINEAR (raw pixels, garbled but no crash).

**Proper fix needed:** V3 helper protocol must pass DMA-BUF fd via SCM_RIGHTS for
non-linear modifiers, so the parent can use EGL GPU deswizzle.

### 5. Multi-monitor and coordinate mapping
```
Affected projects: kmsvnc, RustDesk
Frequency: ~10% of issues
Cause: cursor coordinate space vs CRTC vs plane
```

### 6. Double-buffering / frame skip
```
Affected projects: kmsvnc
Frequency: low but critical
Cause: caching fb_id instead of refreshing it each frame
```

### 7. Kernel/libdrm version compatibility
```
Affected projects: kmsvnc, FFmpeg
Frequency: ~10%
Cause: new APIs (GetFB2, pidfd_getfd, format modifiers)
Solution: #ifdef guards + fallback paths
```

---

## Gotcha Checklist for libdrmtap

From reading all these issues, the library MUST:

- [ ] Support AR30/XR30 (10-bit) and AB48/ABGR16161616 (16-bit HDR)
- [ ] Refresh `plane->fb_id` on every frame (never cache)
- [ ] Detect `handles[0] == 0` and activate helper automatically
- [ ] Handle non-zero offsets in mmap (ARM SoCs)
- [ ] Have fallback when VAAPI reports "not implemented"
- [ ] Try multiple `/dev/dri/cardN` if the first has no active planes
- [ ] Read `panel orientation` property from connector
- [ ] Define minimum kernel (5.6+) and add `#ifndef` guards
- [ ] Use MIT/BSD license (not GPL)
- [ ] Handle reconnection when framebuffers are destroyed and recreated
- [ ] Implement Prime pipeline (not GEM_FLINK) for vkms compatibility
- [ ] Document that NixOS/Flatpak need special configuration for the helper
- [ ] Do NOT implement damage tracking — FB_DAMAGE_CLIPS is for writers (compositors), not readers. Integrators (RustDesk, VNC) do their own frame diff
- [ ] Support continuous capture as a simple polling loop — no callbacks, no epoll required
- [ ] Protect `drmtap_ctx` with `pthread_mutex_t` for thread safety (or document one-ctx-per-thread pattern)
- [ ] Verify coexistence: multiple DRM readers (NoMachine, Sunshine, kmsvnc) can capture simultaneously
- [ ] Helper binary path must be configurable (compile-time + runtime) — distros use `/usr/libexec/`, not `/usr/local/bin/`
- [ ] Set `FD_CLOEXEC` on received DMA-BUF fds to prevent leaking to child processes
- [ ] Handle helper crash recovery — auto-restart if helper dies mid-capture
- [x] **Never CPU-deswizzle CCS modifiers** — `dumb_mmap` returns compressed data, only EGL can deswizzle. Return `-ENOTSUP` and require DMA-BUF fd + EGL (commit 7c7ce27)
- [ ] **V3 protocol: pass DMA-BUF fd via SCM_RIGHTS** for non-linear modifiers so parent can use EGL GPU deswizzle (needed for Intel CCS in unprivileged mode)

