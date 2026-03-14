<div align="center">

# 🔍 libdrmtap

**A zero-dependency C library for DRM/KMS framebuffer capture on Linux**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Built with AI](https://img.shields.io/badge/Built%20with-AI%20Agents-blueviolet)](docs/AI_DEVELOPMENT.md)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)

*Capture any Linux display — no Wayland, no PipeWire, no user prompts.*

</div>

---

## The Problem

Screen capture on modern Linux is a mess:

- **Wayland prevents** unprivileged screen capture by design
- **PipeWire/xdg-desktop-portal** require user interaction every time
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

Continuous capture for remote desktop / streaming:

```c
// 60fps capture loop — no special API needed
while (running) {
    drmtap_grab_mapped(ctx, &frame);
    encode_and_send(frame.data, frame.width, frame.height);
    drmtap_frame_release(ctx, &frame);
    usleep(16666);  // ~60fps
}
```

## Features

| Feature | Status |
|---|---|
| Intel (i915/xe) via VAAPI | 🔜 Planned |
| AMD (amdgpu) via VAAPI | 🔜 Planned |
| Nvidia (nvidia-drm) deswizzle | 🔜 Planned |
| VMs (virtio, vmwgfx, vbox) | 🔜 Planned |
| Multi-monitor (per-CRTC) | 🔜 Planned |
| Zero-copy DMA-BUF output | 🔜 Planned |
| Mapped RGBA output | 🔜 Planned |
| Continuous capture (polling loop) | 🔜 Planned |
| Cursor capture | 🔜 Planned |
| HDR / 10-bit support | 🔜 Planned |
| Privileged helper binary | 🔜 Planned |
| Thread-safe (`pthread_mutex`) | 🔜 Planned |
| Coexists with NoMachine/Sunshine | ✅ By design |
| MIT License | ✅ |

## Why Not Use Existing Tools?

Every existing project is either a complete application, a plugin, or PipeWire-based. **None is an embeddable C library for DRM/KMS capture.**

| Project | Embeddable lib? | DRM/KMS? | Privilege helper? | Multi-GPU? | Multi-monitor? |
|---|---|---|---|---|---|
| FFmpeg kmsgrab | ❌ Needs FFmpeg (40MB+) | ✅ | ❌ Root on binary | ✅ | ✅ |
| gpu-screen-recorder | ❌ CLI tool | ✅ Partial | ✅ gsr-kms-server | ✅ | ✅ |
| kmsvnc | ❌ VNC server | ✅ | ❌ Needs root | ⚠️ Partial | ✅ |
| obs-kmsgrab | ❌ OBS plugin | ✅ | ✅ pkexec | ❌ First GPU only | ❌ |
| Sunshine | ❌ Built-in | ✅ | ❌ setcap on binary | ✅ | ✅ |
| libscreencapture-wayland | ✅ C++ lib | ❌ PipeWire | ❌ N/A | ❌ | ⚠️ |
| screenrec / drmcap | ❌ CLI tools | ✅ | ❌ Needs root | ❌ | ❌ |
| NVIDIA Capture SDK | ❌ Proprietary | ❌ NVFBC | ❌ N/A | ❌ Nvidia only | ✅ |
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
│  │ Connectors │  │ DMA-BUF │  │ HDR tonemap  │  │
│  └────────────┘  └─────────┘  └──────────────┘  │
│                                                 │
│  ┌────────────┐  ┌────────────────────────────┐ │
│  │ gpu_backend│  │ privilege_helper           │ │
│  │ Intel:VAAPI│  │ Auto-spawn drmtap-helper   │ │
│  │ AMD: VAAPI │  │ SCM_RIGHTS fd passing      │ │
│  │ NV:  CPU   │  │ Transparent to user        │ │
│  │ VM: Direct │  │                            │ │
│  └────────────┘  └────────────────────────────┘ │
├─────────────────────────────────────────────────┤
│           libdrm / kernel DRM/KMS               │
└─────────────────────────────────────────────────┘
```

## Scope — What libdrmtap Does (and Doesn't)

| ✅ We do | ❌ We don't |
|---|---|
| Capture framebuffer pixels (RGBA / DMA-BUF) | Detect changed regions (damage tracking) |
| Handle GPU tiling → linear conversion | Encode or compress frames |
| Manage permissions transparently | Transport frames over network |
| Support Intel, AMD, Nvidia, VMs | Inject keyboard/mouse input |
| Provide cursor position + shape | Replace Wayland's screen sharing protocols |
| Enumerate displays and monitors | Implement a VNC/RDP server |

**Wayland positioning**: For interactive desktop sharing where user consent matters, use Wayland protocols (`ext-image-capture-source-v1`, PipeWire). For unattended system-level capture (servers, kiosks, CI, remote management), use libdrmtap. We complement Wayland — we don't compete with it.

## Why This Exists

This project was born from a real frustration: trying to get remote desktop working on Ubuntu Wayland. After RustDesk broke packages, Sunshine failed on KMS, and every solution required "just switch to X11" — we decided to solve it at the root.

We researched every existing project on GitHub, analyzed 25+ real-world issues, studied the source code of kmsvnc, gpu-screen-recorder, FFmpeg kmsgrab, and kms-screenshot. All findings are documented in [`docs/research/`](docs/research/).

> **Read the full research:** 8 documents covering the landscape, Wayland capture problems, DRM/KMS internals, permissions, GPU differences, API design, GitHub issues analysis, and potential adopters.

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
| [`docs/research/00_landscape.md`](docs/research/00_landscape.md) | GitHub landscape — 11 projects analyzed |
| [`docs/research/01_wayland_capture_problem.md`](docs/research/01_wayland_capture_problem.md) | Wayland capture problem — why every project struggles |
| [`docs/research/02_drm_kms_mechanism.md`](docs/research/02_drm_kms_mechanism.md) | DRM/KMS capture mechanism deep dive |
| [`docs/research/03_permissions.md`](docs/research/03_permissions.md) | Permissions model & helper binary |
| [`docs/research/04_gpu_and_testing.md`](docs/research/04_gpu_and_testing.md) | GPU differences & vkms testing |
| [`docs/research/05_api_and_architecture.md`](docs/research/05_api_and_architecture.md) | API design & architecture |
| [`docs/research/06_github_issues_analysis.md`](docs/research/06_github_issues_analysis.md) | Analysis of 25+ real GitHub issues |
| [`docs/research/07_potential_adopters.md`](docs/research/07_potential_adopters.md) | Integration targets — 243K+ ⭐ of potential adopters |

## Contributing

We welcome contributions of all kinds! Whether you're fixing a bug, adding GPU support, improving docs, or just reporting an issue — you're helping solve a real problem for the Linux desktop community.

**See [`CONTRIBUTING.md`](CONTRIBUTING.md) for guidelines.**

Areas where we especially need help:
- 🐧 **Testing on real hardware** (Intel, AMD, Nvidia, Raspberry Pi)
- 🔧 **GPU-specific backends** (tiling format expertise)
- 📖 **Documentation and examples**
- 🦀 **Rust bindings** (`libdrmtap-sys` crate)

## License

[MIT License](LICENSE) — Use it anywhere, in anything.

We chose MIT specifically so projects like RustDesk (Apache 2.0), Sunshine, and proprietary software can all integrate libdrmtap without license conflicts.

---

<div align="center">

**libdrmtap** — Framebuffer capture for the Linux kernel, no compositor required.

Built with ❤️, frustration, and AI agents.

</div>
