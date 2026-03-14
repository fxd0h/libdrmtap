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
libdrmtap = "0.1"
```

```rust
use libdrmtap::DrmTap;

let mut tap = DrmTap::open(None)?;
let frame = tap.grab_mapped()?;
println!("{}x{} pixels captured", frame.width(), frame.height());
```

## Features

| Feature | Status |
|---|---|
| VMs (virtio_gpu, Parallels, QEMU) | ✅ Verified (57 FPS) |
| Multi-monitor (per-CRTC) | ✅ Implemented |
| Zero-copy DMA-BUF output | ✅ Implemented |
| Mapped RGBA output | ✅ Verified |
| Continuous capture (polling loop) | ✅ 57 FPS verified |
| Cursor capture (position + pixels) | ✅ Verified |
| Privileged helper (setcap, no root) | ✅ Verified |
| Security hardening (cap drop + seccomp) | ✅ Implemented |
| EGL/GLES2 GPU-universal detiling | ✅ Implemented (all GPUs) |
| Intel (i915/xe) X/Y-tiled deswizzle | ✅ CPU fallback |
| AMD (amdgpu) deswizzle | ✅ CPU fallback |
| Nvidia (nvidia-drm) blocklinear deswizzle | ✅ CPU fallback |
| HDR / 10-bit (AR30/XR30 → XRGB) | ✅ Implemented |
| Frame differencing (dirty rects) | ✅ Implemented |
| Thread-safe (`pthread_mutex`) | ✅ Implemented |
| Coexists with NoMachine/Sunshine | ✅ By design |
| Rust bindings ([crates.io](https://crates.io/crates/libdrmtap)) | ✅ Published |
| MIT License | ✅ |

> ⚠️ **Testing status**: Capture pipeline verified on `virtio_gpu` (QEMU/Parallels VMs).
> Intel, AMD, and Nvidia GPU backends are implemented but await real hardware testing.
> If you test on real hardware, please [report results](https://github.com/fxd0h/libdrmtap/issues).

## Quick Start

### Build

```bash
# Dependencies: meson, gcc, libdrm-dev
sudo apt install meson gcc libdrm-dev pkg-config

# Optional (for EGL GPU-universal detiling):
sudo apt install libegl-dev libgles2-mesa-dev

# Build
git clone https://github.com/fxd0h/libdrmtap.git
cd libdrmtap
meson setup build
meson compile -C build
```

### Install

```bash
sudo meson install -C build
```

### Set up privileged helper (for capture without root)

```bash
# Install the helper with CAP_SYS_ADMIN
sudo setcap cap_sys_admin+ep /usr/local/libexec/drmtap-helper
```

### Take a screenshot

```bash
# With sudo (direct capture):
sudo ./build/screenshot > screenshot.ppm

# Without sudo (via helper — after setcap above):
./build/screenshot > screenshot.ppm
```

### Run tests

```bash
# Unit tests (no hardware needed)
meson test -C build --suite unit

# Integration tests (needs DRM device)
sudo DRM_DEVICE=/dev/dri/card0 meson test -C build --suite integration
```

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
│          Public API — ~10 functions             │
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
│  │ EGL: All   │  │ Auto-spawn drmtap-helper   │ │
│  │ Intel: CPU │  │ SCM_RIGHTS fd passing      │ │
│  │ AMD:  CPU  │  │ cap_drop + seccomp         │ │
│  │ NV:   CPU  │  │ Transparent to user        │ │
│  │ VM: Linear │  │                            │ │
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
| Enumerate displays and monitors | |
| Detect changed regions (dirty rects) | |

**Wayland positioning**: For interactive desktop sharing where user consent matters, use Wayland protocols (`ext-image-capture-source-v1`, PipeWire). For unattended system-level capture (servers, kiosks, CI, remote management, **login screens**), use libdrmtap. We complement Wayland — we don't compete with it.

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

**See [`docs/AI_DEVELOPMENT.md`](docs/AI_DEVELOPMENT.md) for our full AI development philosophy and methodology.**

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
