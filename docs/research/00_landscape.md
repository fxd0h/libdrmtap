# Landscape Research — DRM/KMS Screen Capture on Linux

> **Date**: 2026-03-14  
> **Goal**: Determine if an embeddable C library for framebuffer capture via DRM/KMS (without user permission) already exists

---

## Conclusion

**The niche is empty.** No embeddable C library with a clean public API exists for framebuffer capture via DRM/KMS without Wayland/PipeWire/xdg-desktop-portal dependencies.

The closest projects are **full applications** (kmsvnc, gpu-screen-recorder) that contain DRM capture logic but don't expose it as a reusable library.

> **Update (libdrmtap)** — This snapshot's hypothesis held: the gap was real, and `libdrmtap` now fills it as the embeddable, MIT-licensed C library described here. It adopts the privileged-helper pattern first observed in gpu-screen-recorder (a small `drmtap-helper` carrying `CAP_SYS_ADMIN` via file capabilities, talking to the unprivileged library over a `socketpair`), and goes further with a zero-copy DMA-BUF + `SCM_RIGHTS` capture path (V3) and a GPU-universal EGL/GLES2 detiling backend. Verified on Intel i915/xe, Nvidia (incl. Tegra/Jetson, aarch64) and virtio-gpu; AMD `amdgpu` is implemented but not yet hardware-verified. HDR10 / 10-bit scanouts are tone-mapped to SDR ([#16](https://github.com/fxd0h/libdrmtap/issues/16), done): PQ decode, BT.2020 → BT.709 gamut, a highlight-preserving curve, for `AR30`/`XR30` and 16-bit `XR48`/`AR48` in the CPU and EGL paths (`P010` overlay YUV and HLG excepted).

---

## Projects Using Direct DRM/KMS

| Project | Language | Type | Library? | Status | Last Activity |
|---|---|---|---|---|---|
| [kmsvnc](https://github.com/isjerryxiao/kmsvnc) | C | DRM/KMS VNC server | ❌ Full app | Functional, "early stage" | Jul 2024 |
| [signal-slot/kmsvnc](https://github.com/signal-slot/kmsvnc) | C++/Qt | kmsvnc rewrite | ❌ Full app | New, active | Mar 2026 |
| [kmsvnc-rs](https://github.com/icepie/kmsvnc-rs) | Rust | kmsvnc port | ❌ Full app | Very new | Mar 2026 |
| [gpu-screen-recorder](https://git.dec05eba.com/gpu-screen-recorder) | C | GPU screen recorder | ❌ Full app | Very popular, active | 2025+ |
| [kms-screenshot](https://github.com/thmasq/kms-screenshot) | C | Simple screenshot | ❌ Single script | Minimal, ~1 file | Low activity |
| [drmScreenshot](https://github.com/guolele1990/drmScreenshot) | C | DRM/KMS screenshot | ❌ Single script | 1 commit, abandoned | Jun 2022 |
| [wl-screenshare](https://github.com/giammirove/wl-screenshare) | C | Screenshare via DRM | ❌ Full app | Fork of gpu-screen-recorder | ~2024 |
| FFmpeg `kmsgrab.c` | C | FFmpeg input device | ❌ Embedded in FFmpeg | Maintained | Active |

## Projects NOT Using Direct DRM/KMS

| Project | Language | Mechanism | Library? | Problem |
|---|---|---|---|---|
| [libscreencapture-wayland](https://github.com/DafabHoid/libscreencapture-wayland) | C++ | xdg-desktop-portal | ✅ | Requires user permission |
| [wf-recorder](https://github.com/ammen99/wf-recorder) | C++ | wlr-screencopy | ❌ App | wlroots-only, not direct DRM |
| [wl-screenrec](https://github.com/russelltg/wl-screenrec) | Rust | wlr-screencopy / DMA-BUF | ❌ App | wlroots-only, Rust |
| RustDesk | Rust | PipeWire + xdg-portal | ❌ App | Requires user permission on Wayland |
| Sunshine | C++ | KMS + Wayland fallback | ❌ App | Full streaming app, not a library |

---

## Analysis of Most Relevant Projects

### kmsvnc (isjerryxiao/kmsvnc)
- VNC server capturing via DRM/KMS. Works on X11, Wayland, and Linux VT
- Key code: `drm.c` — uses `drmModeGetFB2`, `drmPrimeHandleToFD`, imports as texture via VAAPI
- Dependencies: cmake, libvncserver, libxkbcommon, libdrm, libva
- Documented quirk: Intel CCS requires `INTEL_DEBUG=noccs`; Nvidia experimental
- **Not a competitor**: full app, doesn't separate capture as a library

### gpu-screen-recorder (dec05eba)
- High-performance GPU screen recorder. AMD/Intel/Nvidia
- Has `kms/server/kms_server.c` — separate process with elevated permissions that captures via DRM and passes buffers to main process
- Code at git.dec05eba.com (not on GitHub), flathub has the flatpak
- **Implements the privileged helper pattern** described in our proposal
- **Not a competitor**: full video recording app, not a library

### FFmpeg kmsgrab.c
- FFmpeg input device for DRM/KMS capture
- Reference code battle-tested by millions of users
- Buried inside FFmpeg, not extractable as a standalone library

### kms-screenshot (thmasq)
- Single C file (~400 lines) that captures one DRM/KMS frame → file
- Functional proof-of-concept, confirms viability of the approach
- Doesn't handle multiple GPUs, has no API, doesn't solve permissions

---

## Market Signals

**Growing demand confirmed:**
1. `kmsvnc-rs` created 3 days ago (Mar 2026) — people replicating the idea
2. `signal-slot/kmsvnc` created 1 month ago (Feb 2026) — active C++ rewrite
3. RustDesk still hasn't solved unattended access on Wayland
4. Sunshine has open issues about KMS capture on Ubuntu Wayland
5. RustDesk discussions (Dec 2024) explore `kmsgrab` as PipeWire alternative

**Gap confirmed:**
- No project exposes DRM/KMS as an embeddable C library with a public API
- Existing ones are full apps or single-file scripts
- `libscreencapture-wayland` is the only library, but uses xdg-desktop-portal

---

## Technical Reference Files to Study

| Priority | Project | What to Study | Key File |
|---|---|---|---|
| 1 | kmsvnc | Full DRM implementation in C | `drm.c` |
| 2 | gpu-screen-recorder | Privileged helper + multi-GPU | `kms/server/kms_server.c` |
| 3 | FFmpeg | Production-proven implementation | `libavdevice/kmsgrab.c` |
| 4 | kms-screenshot | Minimal DRM capture PoC | `kms-screenshot.c` |
