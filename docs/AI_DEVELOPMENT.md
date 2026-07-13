# 🤖 AI-Assisted Development

## Philosophy

libdrmtap is built using AI coding agents as first-class development tools. We believe this represents the future of open-source development, and we're transparent about it.

**We don't hide AI usage — we document it, celebrate it, and encourage it.**

## Why AI Agents?

The DRM/KMS screen capture problem requires deep knowledge across multiple domains:

- Linux kernel internals (DRM subsystem, ioctls, DMA-BUF)
- GPU driver specifics (Intel, AMD, Nvidia — each completely different)
- Security models (CAP_SYS_ADMIN, DRM Master, Unix sockets, SCM_RIGHTS)
- Pixel format handling (tiling, deswizzle, HDR tone mapping)
- Cross-referencing dozens of existing projects, issues, and kernel docs

A single developer researching all of this manually would take weeks. With AI agents, the research phase — analyzing 11 projects, reading 25+ GitHub issues, studying 3 reference codebases line by line — was completed in hours.

## Tools Used

### Antigravity (Google DeepMind)
- **Role**: Primary agentic coding assistant
- **Used for**: Codebase analysis, GitHub issue research, architecture design, documentation writing, code generation
- **Why**: Full agent capabilities — can browse GitHub, read source code, analyze issues, and produce structured technical documents

### Gemini (Google DeepMind)
- **Role**: Research and analysis engine
- **Used for**: Web searches, cross-referencing kernel documentation, understanding DRM APIs
- **Why**: Strong reasoning over technical documentation and API specifications

### Claude (Anthropic)
- **Role**: Code generation and technical reasoning
- **Used for**: Code generation, API design review, documentation structure
- **Why**: Excellent at generating clean, well-structured C code and technical writing

## What AI Did

### Research Phase (documented in `docs/research/`)
1. **Landscape analysis** — Found and analyzed 11 DRM/KMS projects on GitHub
2. **RustDesk deep dive** — Traced 4 years of Wayland issues, PRs, and discussions
3. **Source code analysis** — Read `kmsvnc/drm.c` (~800 lines), `kms-screenshot.c` (~1500 lines), FFmpeg `kmsgrab.c` line by line
4. **Issue autopsy** — Read all 25 kmsvnc issues, categorized patterns, extracted gotchas
5. **API design** — Compared 3 implementations, proposed unified API
6. **Architecture** — Designed the helper binary pattern based on gpu-screen-recorder's approach

### Implementation Phase (shipped — C library 0.4.4)
Guided by the research findings, the agents went on to build the library itself. Every architectural decision is traceable to a specific finding in the research docs. What shipped:

- 🧩 **Two-process architecture** — an unprivileged library plus a small privileged `drmtap-helper` that carries `CAP_SYS_ADMIN` via file capabilities and talks over a socketpair, so the main process never has to run privileged.
- 🖥️ **Multi-GPU capture** — Intel i915/xe (verified on dual-4K Meteor Lake), Nvidia `nvidia-drm` including Tegra/Jetson (verified on Orin Nano, aarch64), and virtio-gpu (verified). AMD `amdgpu` is verified on real hardware (RX Vega 64, gfx9), with a generic linear fallback for other drivers.
- ⚡ **V3 and V2 capture paths** — V3 (default) exports the scanout as a DMA-BUF and passes the fd over `SCM_RIGHTS` for zero-copy capture; V2 falls back to dumb-mapping and copying the pixels over the socket when DMA-BUF export isn't possible.
- 🎨 **GPU-universal EGL detiling** (`gpu_egl.c`) — imports tiled/compressed framebuffers (Intel X/Y-tiled + CCS, AMD, Nvidia block-linear, vendor modifiers) as an EGLImage and uses GLES2 + `glReadPixels` to produce linear RGBA; a CPU deswizzle exists as a fallback for some formats. Plain linear framebuffers are mapped directly with no detile.
- 🔒 **A hardened helper** — verifies it was spawned by the library and checks the peer uid via `SO_PEERCRED`, restricts the DRM device to a realpath under `/dev/dri/` opened `O_RDONLY`, sets `PR_SET_NO_NEW_PRIVS`, drops all capabilities except `CAP_SYS_ADMIN` (libcap), and installs a default-KILL seccomp allowlist (libseccomp) that deliberately forbids `open`/`openat`. Built with stack-protector-strong, FORTIFY, PIE and full RELRO.

### Status

- 🌈 **HDR10 (#16, done)** — HDR scanouts are tone-mapped to SDR: PQ (ST 2084) decode, BT.2020 → BT.709 gamut, a highlight-preserving curve and sRGB, for `AR30`/`XR30` and 16-bit `XR48`/`AR48`/`XB48`/`AB48`, in both the CPU and EGL (tiled) paths, driven by the connector `HDR_OUTPUT_METADATA`. HLG falls back to a plain reduction, and `P010` (overlay-video YUV) is not handled.
- 🪟 **virgl integration (#15, done)** — host-rendered virtio-gpu 3D scanouts (black to a guest CPU mmap) are captured via GPU-side EGL readback on the guest GPU, verified end-to-end in a virgl VM.
- 🔗 **RustDesk integration (open)** — a DRM capture backend for RustDesk's `scrap` (depending on `libdrmtap-sys`) to avoid the Wayland portal consent dialog. Upstream PR [`rustdesk/rustdesk#15420`](https://github.com/rustdesk/rustdesk/pull/15420) is under maintainer review — the security hardening was praised — but it is **not** merged yet.

## What AI Did NOT Do

- **Make final decisions** — A human developer reviewed and approved all designs
- **Test on real hardware** — AI can't access GPU hardware
- **Replace understanding** — The human developer understands every line of the research and code
- **Work unsupervised** — Every step was reviewed, questioned, and refined through human-AI dialogue

## How to Contribute with AI

If you use AI tools (Copilot, ChatGPT, Claude, Gemini, or others) to help with your contributions:

1. ✅ **Go for it** — We encourage AI-assisted contributions
2. ✅ **Understand your code** — Don't submit code you can't explain
3. ✅ **Test it** — AI-generated code needs the same testing as human-written code
4. ✅ **Credit the tool** — Optionally mention in your PR if AI was used (not required)
5. ❌ **Don't auto-generate and submit** — Review, understand, then submit

## The Thesis

> A single developer with AI agents can produce research and code that would traditionally require a team of specialists.

This project is living proof of that thesis. The entire research corpus (9 documents, ~1,900 lines of technical analysis) was produced by one human + AI agents, and the shipped 0.4.4 library grew directly out of it.

We hope this inspires other open-source developers to embrace AI as a tool — not a replacement, but an amplifier of human capability.

---

*"The best tool is the one that makes you more effective without making you less thoughtful."*
