<div align="center">

# 🔍 libdrmtap

**A zero-dependency C library for DRM/KMS framebuffer capture on Linux**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![crates.io](https://img.shields.io/crates/v/libdrmtap.svg)](https://crates.io/crates/libdrmtap)
[![Built with AI](https://img.shields.io/badge/Built%20with-AI%20Agents-blueviolet)](docs/AI_DEVELOPMENT.md)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)

*Capture any Linux display — no Wayland, no PipeWire, no user prompts.*

</div>

---

## The Problem

Screen capture on modern Linux is a mess:

- **Wayland prevents** unprivileged screen capture by design
- **PipeWire/xdg-desktop-portal** require user interaction every time
- **Login screens** (GDM, SDDM) can't be captured at all — no compositor running
- **RustDesk, Sunshine, and others** have fought this for 4+ years with no clean solution
- **No embeddable C library** exists for direct framebuffer capture

## The Solution

**libdrmtap** goes below the compositor — directly to the kernel's DRM/KMS subsystem — to capture the framebuffer without any user interaction, compositor cooperation, or display server dependency.

```c
#include <drmtap.h>

int main() {
    drmtap_ctx *ctx = drmtap_open(NULL);  // auto-detect GPU
    
    drmtap_frame_info frame;
    drmtap_grab_mapped(ctx, &frame);      // capture screen → RGBA
    
    // frame.data = linear RGBA pixels
    // frame.width, frame.height, frame.stride
    
    drmtap_frame_release(ctx, &frame);
    drmtap_close(ctx);
}
```

### Rust

```toml
[dependencies]
libdrmtap = "0.5"
```

> This pulls in `libdrmtap-sys`, which embeds and statically compiles the
> C sources (and the privilege helper). No system `libdrmtap` install, no
> `meson install`, no `pkg-config` to find a shared library.

```rust
use libdrmtap::DrmTap;

let mut tap = DrmTap::open(None)?;
let frame = tap.grab_mapped()?;
println!("{}x{} pixels captured", frame.width(), frame.height());
```

## Features

| Feature | Status |
|---|---|
| VMs (virtio_gpu, Parallels, QEMU) | ✅ Verified |
| Multi-monitor (per-CRTC) | ✅ Implemented |
| Multi-GPU (enumerate every card + bind convert to the exporting GPU) | ✅ Implemented |
| Split capture (privileged export + unprivileged convert) | ✅ Implemented |
| Zero-copy DMA-BUF output (V3) | ✅ Implemented |
| Mapped RGBA output | ✅ Verified |
| Continuous capture (polling loop) | ✅ Verified |
| Cursor capture (position + pixels) | ✅ Verified (on a para-virtualized driver, needs 0.5.4 — see Known Limitations) |
| Privileged helper (setcap, no root) | ✅ Verified |
| Security hardening (cap drop + seccomp) | ✅ Implemented |
| EGL/GLES2 GPU-universal detiling | ✅ Implemented (primary, all GPUs) |
| Intel (i915/xe) X/Y-tiled deswizzle | ✅ CPU fallback |
| AMD (amdgpu) deswizzle | ⛔ EGL only — no CPU decoder, fails closed |
| Nvidia (nvidia-drm) blocklinear deswizzle | ⛔ EGL only — there is no CPU decoder; the CPU path returns `-ENOTSUP` (0.5.3) |
| HDR10 → SDR tone-map (AR30/XR30/AB30/XB30, XR48/AR48/XB48/AB48) | ✅ Implemented (P010 not yet) |
| Frame differencing (dirty rects) | ✅ Implemented |
| Thread-safe (one `drmtap_ctx` per thread) | ✅ By design |
| Coexists with NoMachine/Sunshine | ✅ By design |
| Rust bindings ([crates.io](https://crates.io/crates/libdrmtap)) | ✅ Published |
| MIT License | ✅ |

> ⚠️ **Testing status**: every entry below is a machine someone captured from,
> and it says whose. A tester confirmation is one host on one day, not a support
> matrix.
>
> **Verified here:**
> - **Intel Meteor Lake-P** (`i915`, Core Ultra 7 155H) — multiple displays up to
>   3840x2160, `4_TILED_MTL_RC_CCS_CC` scanout detiled through EGL.
> - **AMD RX560** (Polaris/gfx8) — 2880x1800 `eDP-1`, tiled `XR30`, EGL detile.
> - **NVIDIA Jetson Orin Nano** (`nvidia-drm`, aarch64, Wayland).
> - **`virtio_gpu`** (QEMU/Parallels VMs), V3 zero-copy path.
>
> **Confirmed by outside testers, on their hardware and not ours:**
> - **AMD RX Vega 64** (gfx9), X11, modifier `0x200000004801a01`, unprivileged via
>   the helper — GK-Gaming, 2026-07-29
>   ([#26](https://github.com/fxd0h/libdrmtap/issues/26),
>   [#36](https://github.com/fxd0h/libdrmtap/issues/36)).
> - **Intel Raptor Lake with a hybrid NVIDIA GPU** (ThinkBook 16p G5 IRX, Ubuntu
>   26.04, GNOME Wayland, `eDP-1`, `Y_TILED_GEN12_RC_CCS_CC`) — huzhifeng,
>   2026-08-07 ([#45](https://github.com/fxd0h/libdrmtap/issues/45)).
>
> Whichever GPU you have, most of this depends on the EGL detile backend. The
> CPU path on its own still handles linear scanouts and the plain tilings (Intel
> X/Y/Yf-tiled), but a **compressed** scanout, or any tiled
> layout it has no deswizzler for, **fails closed** — it returns an error rather
> than passing the raw bytes off as linear pixels. Current Intel and AMD desktops
> scan out exactly those. The `egl` Meson feature defaults to `auto`, which
> silently builds the CPU-only stub when the headers are missing, so configure
> with `-Degl=enabled` to fail at configure time instead — that is what
> [#45](https://github.com/fxd0h/libdrmtap/issues/45) turned out to be.
>
> **Multi-GPU render-node selection is verified on the Jetson Orin's two DRM
> devices** (`tegra`/renderD128 with no connectors + `nvidia-drm`/renderD129
> driving the display): the converter now binds renderD129, the node that
> exported the scanout.
> **Multi-card, multi-VENDOR hosts with more than one display-driving GPU are now
> verified** on an Apple T2 MacBook Pro: three DRM cards — `appletbdrm` (the Touch
> Bar strip), `i915` (no connected output) and `amdgpu` (the panel) — where two
> different vendors are each scanning out a display at once, and each was captured
> from its own card. That machine also covers two cases nothing else here does:
>
> - **A display-only card with NO render node.** `appletbdrm` exposes an active
>   CRTC and no `renderD*`, so a consumer cannot be told which GPU exported the
>   scanout and must not guess: importing it on the wrong device can corrupt the
>   image silently. Such a frame takes the CPU path instead.
> - **A scanout framebuffer PADDED wider than the mode.** That card drives a
>   60x2170 mode out of a 64x2170 framebuffer (256-byte pitch), and those four
>   columns are alignment padding that is never scanned out. A grab reports the
>   width the CRTC scans out, not the framebuffer's, so a consumer that enumerated
>   the mode is not handed an image wider than the display it asked for. Worth
>   knowing if you compare a frame's geometry against a display list.
> If you test on real hardware, please [report results](https://github.com/fxd0h/libdrmtap/issues).
>
> ℹ️ **virgl note**: Plain `virtio-gpu` is captured with a direct linear map. A
> host-rendered **virgl** (3D) scanout cannot be read by the guest CPU (it comes
> out black, and `TRANSFER_FROM_HOST` does not bring it back); it is captured via
> GPU-side EGL readback on the guest. Solved technically; production integration is
> in progress ([#15](https://github.com/fxd0h/libdrmtap/issues/15)).
>
> **HDR10 capture is tone-mapped to SDR.** When the connector advertises an HDR
> scanout (`HDR_OUTPUT_METADATA`, PQ), libdrmtap applies the real transfer —
> PQ (SMPTE ST 2084) decode, BT.2020 → BT.709 gamut, a highlight-preserving
> tone-map, sRGB — and returns correct 8-bit SDR, instead of the washed-out
> top-8-of-10-bits result. This matches what consumers like RustDesk expect
> (their pipeline is 8-bit SDR; on other platforms the OS/compositor tone-maps
> before they see it — on DRM we do it). Covers `AR30`/`XR30`/`AB30`/`XB30`
> (10-bit) and `XR48`/`AR48`/`XB48`/`AB48` (16-bit), in both the CPU and the EGL
> (tiled) paths. `P010` (10-bit YUV, used for overlay-video planes rather than the
> primary desktop scanout) is not handled.
>
> **FP16 half-float scanouts** (`XRGB16161616F` and its BGR/alpha siblings) are
> reduced to 8-bit sRGB on the CPU fallback — linear-light decode through the sRGB
> OETF, not HDR tone-mapped (values above 1.0 clip to white).

## Quick Start

### Requirements

The tiled/compressed framebuffer path (Intel/AMD/Nvidia modifiers) uses the DRM
`GETFB2` ioctl, which needs **Linux 4.20+** — i.e. Ubuntu 20.04 (5.4), 22.04
(5.15), 24.04 (6.8) or newer. Linear framebuffers (virtio-gpu and similar VMs)
work on older kernels.

### Build

```bash
# Dependencies: meson, gcc, libdrm-dev
sudo apt install meson gcc libdrm-dev pkg-config

# Optional (for EGL GPU-universal detiling):
sudo apt install libegl-dev libgles2-mesa-dev

# Build. -Degl=enabled is not optional in practice: the EGL backend is what
# detiles a modern scanout, and without it meson quietly builds a stub that
# fails on the first real frame. Passing it turns that into a configure error.
git clone https://github.com/fxd0h/libdrmtap.git
cd libdrmtap
meson setup build -Degl=enabled
meson compile -C build
```

### Install

```bash
sudo meson install -C build
```

### Set up privileged helper (for capture without root)

A Meson install leaves the helper world-executable. Do **not** apply the
capability to it in that state: a file capability applies to every user who can
`exec` the binary, so `cap_sys_admin+ep` on a `0755` helper lets any local user
read the DRM scanout (login screen, lock screen, other users' sessions). Restrict
who can run it FIRST — `root:<capture-group>`, mode `0750` — then apply the
capability. The `SO_PEERCRED` check inside the helper is defense-in-depth, not an
access-control boundary; the file mode is. This mirrors the procedure in
[`SECURITY.md`](SECURITY.md).

```bash
# Restrict execution to a trusted group, THEN grant the capability.
sudo groupadd -f drmtap-capture
sudo chown root:drmtap-capture /usr/local/libexec/drmtap-helper
sudo chmod 0750               /usr/local/libexec/drmtap-helper
sudo setcap cap_sys_admin+ep  /usr/local/libexec/drmtap-helper
# Add each user who is allowed to capture to the group:
sudo usermod -aG drmtap-capture "$USER"
```

### Take a screenshot

```bash
# With sudo (direct capture):
sudo ./build/screenshot > screenshot.ppm

# Without sudo (via helper — after setcap above):
./build/screenshot > screenshot.ppm
```

### VNC server demo (full remote desktop)

The included VNC server example captures the screen and serves it over VNC
with mouse and keyboard input:

```bash
# Build with libvncserver (optional dependency)
meson setup build -Degl=enabled && meson compile -C build

# Run (needs root for uinput access)
sudo ./build/vnc_server

# Connect from any VNC client:
#   vnc://YOUR_IP:5900  (password: drmtap)
```

### RustDesk integration

libdrmtap is being upstreamed into [RustDesk](https://github.com/rustdesk/rustdesk)
via [rustdesk/rustdesk#15420](https://github.com/rustdesk/rustdesk/pull/15420).
The integration adds a `drm` backend to `scrap` that **dlopens** `libdrmtap.so.0`
at runtime: it does not link libdrmtap at build time and does not depend on the
[`libdrmtap-sys`](https://crates.io/crates/libdrmtap-sys) crate, whose `build.rs`
would statically compile the whole C tree and a privileged helper. Loading the
shared object by soname is what lets a missing or too-old `.so` degrade to the
existing PipeWire path instead of breaking the build.

A self-contained example of the crate-based backend, which does link
`libdrmtap-sys` statically, lives in
[`contrib/integrations/rustdesk/`](contrib/integrations/rustdesk/README.md).
**Tested and verified** on Ubuntu 24.04 with `cargo build` (zero errors).

### Run tests

```bash
# Unit tests (no hardware needed)
meson test -C build --suite unit

# Integration tests (needs DRM device)
sudo DRM_DEVICE=/dev/dri/card0 meson test -C build --suite integration
```

### Environment variables

| Variable | Effect |
|---|---|
| `DRM_DEVICE` | DRM node to open (e.g. `/dev/dri/card0`). **Ignored for privileged (root / `CAP_SYS_ADMIN`) callers** — a privileged capture service uses its configured `device_path` or the KMS auto-scan, so the environment cannot redirect which device it opens. Honored only for unprivileged runs. |
| `DRMTAP_DEBUG` | Set to `1` for verbose debug logging on stderr. |
| `DRMTAP_NO_EGL` | Set to `1` to force the CPU deswizzle/convert path (skip EGL/GLES). |
| `DRMTAP_NO_IMAGE_CACHE` | Set to `1` to re-import the `EGLImage` every frame (disable the import-once cache). |
| `DRMTAP_FORCE_MMAP_FAIL` | Test hook — set to `1` to force the fast-path CPU mmap to fail so the EGL-detile fallback runs. |

The privilege model is described in [`SECURITY.md`](SECURITY.md).

## Performance

CPU cost of the capture loop, measured as a share of one core (approximate; on
Ubuntu 24.04). The library figure is the full grab + detile loop in the consuming
process; the helper figure is the privileged `drmtap-helper` process.

| Environment | Resolution | Capture CPU (1 core) | Helper CPU | Path |
|---|---|---|---|---|
| Intel Meteor Lake (`i915`) | dual 3840×2160 @ 60 | **~16%** | ~0.4% | V3 zero-copy, EGL CCS detile |
| NVIDIA Jetson Orin Nano (`nvidia-drm`) | 1920×1080 | **~11%** | ~0.4% | V3 zero-copy, EGL detile |
| Plain virtio-gpu VM | typical desktop | **~1.2%** | ~0.7% | V3 zero-copy, direct linear map |

> Plain virtio dropped from **~14%** on the old V2 copy path to **~1.2%** once V3
> stopped copying full frames across the socket.

**How it works:**
- **V3 zero-copy (default)** — the privileged helper exports the active scanout as a DMA-BUF and passes the fd back over a `socketpair` via `SCM_RIGHTS`. No full-frame copy crosses the socket.
- **V2 copy (fallback)** — when DMA-BUF export is not possible, the helper dumb-maps the framebuffer and copies the pixels over the socket.
- **GPU EGL readback** — tiled/compressed scanouts (Intel CCS, AMD, Nvidia block-linear, vendor modifiers) and host-rendered virgl buffers are imported as an `EGLImage` and read back linear with `glReadPixels`. Plain linear framebuffers are mapped directly with no detile.
- **Zero-copy DMA-BUF output** — consumers that take the DMA-BUF output can import the buffer directly (e.g. as an `EGLImage` or Vulkan texture) and skip the readback entirely.

## Why Not Use Existing Tools?

Every existing project is either a complete application, a plugin, or PipeWire-based. **None is an embeddable C library for DRM/KMS capture.**

| Project | Embeddable lib? | DRM/KMS? | Privilege helper? | Multi-GPU? | Multi-monitor? |
|---|---|---|---|---|---|
| FFmpeg kmsgrab | ❌ Needs FFmpeg (40MB+) | ✅ | ❌ Root on binary | ✅ | ✅ |
| gpu-screen-recorder | ❌ CLI tool | ✅ Partial | ✅ gsr-kms-server | ✅ | ✅ |
| kmsvnc | ❌ VNC server | ✅ | ❌ Needs root | ⚠️ Partial | ✅ |
| obs-kmsgrab | ❌ OBS plugin | ✅ | ✅ pkexec | ❌ First GPU only | ❌ |
| Sunshine | ❌ Built-in | ✅ | ❌ setcap on binary | ✅ | ✅ |
| ReFrame | ❌ VNC server | ✅ | ✅ Split process | ✅ | ✅ |
| **libdrmtap** | **✅ C library** | **✅** | **✅ Hardened** | **✅** | **✅** |

> **libdrmtap is the only project that is simultaneously**: an embeddable C library, uses DRM/KMS direct capture, includes a hardened privilege helper, supports multiple GPUs, and handles multiple monitors — all under the MIT license.

## Architecture

```
┌─────────────────────────────────────────────────┐
│              YOUR APPLICATION                   │
│      (RustDesk, Sunshine, VNC, custom)          │
├─────────────────────────────────────────────────┤
│              libdrmtap.h                        │
│          Public API — ~20 functions             │
├─────────────────────────────────────────────────┤
│                                                 │
│  ┌────────────┐  ┌─────────┐  ┌──────────────┐  │
│  │ enumerate  │  │  grab   │  │ pixel_convert│  │
│  │ Planes     │  │ GetFB2  │  │ Deswizzle    │  │
│  │ CRTCs      │  │ Prime   │  │ Format cvt   │  │
│  │ Connectors │  │ DMA-BUF │  │ Diff frames  │  │
│  └────────────┘  └─────────┘  └──────────────┘  │
│                                                 │
│  ┌────────────┐  ┌────────────────────────────┐ │
│  │ gpu_backend│  │ privilege_helper           │ │
│  │ EGL: all   │  │ Auto-spawn drmtap-helper   │ │
│  │ (primary)  │  │ DMA-BUF fd via SCM_RIGHTS  │ │
│  │ CPU detile │  │ V3 zero-copy / V2 copy     │ │
│  │ (fallback) │  │ cap_drop + seccomp         │ │
│  │ VM: linear │  │ Transparent to user        │ │
│  └────────────┘  └────────────────────────────┘ │
├─────────────────────────────────────────────────┤
│           libdrm / kernel DRM/KMS               │
└─────────────────────────────────────────────────┘
```

### GPU Detiling Strategy

libdrmtap uses a **dual-path** approach for GPU-tiled framebuffers:

1. **EGL path** (primary) — Imports DMA-BUF as EGLImage with modifier metadata, renders via `GL_TEXTURE_EXTERNAL_OES`. The GPU driver handles its own tiling transparently. One code path for ALL GPUs. Based on [ReFrame](https://github.com/AlynxZhou/reframe).
2. **CPU path** (fallback) — Per-GPU deswizzle when EGL is unavailable (headless servers, no GPU rendering).
3. **Linear path** — VMs and linear framebuffers need no conversion.

## Scope — What libdrmtap Does (and Doesn't)

| ✅ We do | ❌ We don't |
|---|---|
| Capture framebuffer pixels (RGBA / DMA-BUF) | Encode or compress frames |
| Handle GPU tiling → linear conversion | Transport frames over network |
| Manage permissions transparently | Inject keyboard/mouse input |
| Support Intel, AMD, Nvidia, VMs | Replace Wayland's screen sharing protocols |
| Provide cursor position + shape | Implement a VNC/RDP server |
| Enumerate displays and monitors across multiple GPUs | |
| Detect changed regions (dirty rects) | |

**Wayland positioning**: For interactive desktop sharing where user consent matters, use Wayland protocols (`ext-image-capture-source-v1`, PipeWire). For unattended system-level capture (servers, kiosks, CI, remote management, **login screens**), use libdrmtap. We complement Wayland — we don't compete with it.

## Known Limitations

- **Cursor hotspot on bare-metal drivers.** The cursor *image* and *position* are captured exactly, but the cursor **hotspot** (the click point inside the image, e.g. an arrow's tip or an I-beam's centre) is only exposed by the DRM cursor plane on virtualized drivers (`virtio-gpu`, `vmwgfx`), via the `HOTSPOT_X`/`HOTSPOT_Y` plane properties. On bare-metal drivers (i915, amdgpu, nvidia) those properties are absent, and compositors such as Mutter move the cursor with the legacy `drmModeMoveCursor` API — which never updates the atomic `CRTC_X`/`CRTC_Y` plane position either. As a result, on bare metal the hotspot must be *approximated* from the captured image (e.g. the top-left of an arrow's bounding box, the centre of a tall/narrow I-beam), so the rendered cursor can land a few pixels off the true click point. `drmtap_cursor_info.hot_x`/`hot_y` carry the real hotspot when the driver provides it and `0` otherwise; consumers should fall back to an image-derived estimate in that case. The exact position *is* available from the kernel's `/sys/kernel/debug/dri/N/state` debug dump, but that interface is root-only, not a stable ABI, and driver-specific — so it is unsuitable for production use.

  Those same hotspot properties are why, since kernel 6.6, a para-virtualized driver (`virtio-gpu`, `vmwgfx`, `qxl`, `vboxvideo`) **hides its cursor plane entirely** from a client that has enabled `DRM_CLIENT_CAP_ATOMIC` and has not also enabled `DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT`: the second cap is how a client declares it honors them. libdrmtap needs the atomic cap to read a connector's `CRTC_ID`, so it enables both (since 0.5.4 — before it, the cursor read found no plane on those drivers and reported the cursor hidden forever). The hotspot cap is refused with `EOPNOTSUPP` on bare metal, which is harmless and is also a cheap way to tell the two kinds of driver apart.

## Why This Exists

This project was born from a real frustration: trying to get remote desktop working on Ubuntu Wayland. After RustDesk broke packages, Sunshine failed on KMS, and every solution required "just switch to X11" — we decided to solve it at the root.

We researched every existing project on GitHub, analyzed 25+ real-world issues, studied the source code of kmsvnc, gpu-screen-recorder, FFmpeg kmsgrab, ReFrame, and kms-screenshot. All findings are documented in [`docs/research/`](docs/research/).

> **Read the full research:** 9 documents covering the landscape, Wayland capture problems, DRM/KMS internals, permissions, GPU differences, API design, GitHub issues analysis, potential adopters, and the ReFrame EGL analysis.

## 🤖 Built with AI Agents

This project is proudly developed with the assistance of AI coding agents. We don't hide it — we celebrate it.

The entire research phase — analyzing codebases, reading GitHub issues, studying kernel APIs, designing the architecture — was conducted as a collaboration between a human developer and AI agents. This includes:

- **[Antigravity](https://blog.google/technology/google-deepmind/)** by Google DeepMind — agentic AI coding assistant
- **[Gemini](https://deepmind.google/technologies/gemini/)** by Google DeepMind — powering research and analysis
- **[Claude](https://www.anthropic.com/claude)** by Anthropic — powering code generation and reasoning

We believe AI agents are a force multiplier for open source. A single developer with AI assistance can produce research and code that would traditionally require a team. This project is proof of that thesis.

Agent-written code is held to the **same bar as any other contribution**: every PR is reviewed by [CodeRabbit](https://coderabbit.ai), scanned by CodeQL and `cppcheck`, and validated under ASan/UBSan in CI — and the privileged helper survived a serious security audit during RustDesk upstreaming. The hardening was *praised*, not waved through. AI speeds up the work; it does not lower the standard.

**See [`docs/AI_DEVELOPMENT.md`](docs/AI_DEVELOPMENT.md) for our full AI development philosophy, and [`AGENTS.md`](AGENTS.md) for the working agreement we hand to coding agents.**

## Documentation

| Document | Description |
|---|---|
| [`00_landscape.md`](docs/research/00_landscape.md) | GitHub landscape — 11 projects analyzed |
| [`01_wayland_capture_problem.md`](docs/research/01_wayland_capture_problem.md) | Wayland capture problem — why every project struggles |
| [`02_drm_kms_mechanism.md`](docs/research/02_drm_kms_mechanism.md) | DRM/KMS capture mechanism deep dive |
| [`03_permissions.md`](docs/research/03_permissions.md) | Permissions model & helper binary |
| [`04_gpu_and_testing.md`](docs/research/04_gpu_and_testing.md) | GPU differences & vkms testing |
| [`05_api_and_architecture.md`](docs/research/05_api_and_architecture.md) | API design & architecture |
| [`06_github_issues_analysis.md`](docs/research/06_github_issues_analysis.md) | Analysis of 25+ real GitHub issues |
| [`07_potential_adopters.md`](docs/research/07_potential_adopters.md) | Integration targets — 243K+ ⭐ of potential adopters |
| [`08_reframe_egl_analysis.md`](docs/research/08_reframe_egl_analysis.md) | ReFrame analysis — EGL GPU-universal detiling |

## Contributing

We welcome contributions of all kinds! Whether you're fixing a bug, adding GPU support, improving docs, or just reporting an issue — you're helping solve a real problem for the Linux desktop community.

**See [`CONTRIBUTING.md`](CONTRIBUTING.md) for guidelines.**

Areas where we especially need help:
- 🐧 **Testing on real hardware** (Intel, AMD, Nvidia, Raspberry Pi) — validating EGL detiling and CPU fallback paths
- 🔧 **Login screen capture testing** — GDM/SDDM/LightDM on various distros
- 📖 **Documentation and examples** — tutorials, man pages
- 🌐 **Integration patches** — RustDesk, Sunshine, and other remote desktop projects

## License

[MIT License](LICENSE) — Use it anywhere, in anything.

We chose MIT specifically so projects like RustDesk (AGPLv3), Sunshine, and proprietary software can all integrate libdrmtap without license conflicts.

---

<div align="center">

**libdrmtap** — Framebuffer capture for the Linux kernel, no compositor required.

Built with ❤️, frustration, and AI agents.

</div>
