# The Wayland Screen Capture Problem — Why Every Project Struggles

> **Date**: 2026-03-14  
> **Sources**: GitHub issues/PRs from RustDesk, Sunshine, wayvnc, GNOME Remote Desktop, OBS, FFmpeg

---

## Summary

Wayland's security model **breaks screen capture for every remote desktop and streaming project on Linux**. Since Wayland prevents applications from reading other windows' content, projects must use PipeWire + xdg-desktop-portal, which requires user interaction and is unreliable across compositors. No project has solved unattended screen capture on Wayland cleanly. This is the gap libdrmtap fills.

---

## The Core Problem

```
X11 (worked):     App → XGetImage() → screen pixels     ✅ Simple, universal

Wayland (broken): App → xdg-desktop-portal → user prompt → PipeWire → GStreamer → pixels
                                                ↑
                                         Requires human click.
                                         Different behavior per compositor.
                                         Breaks on updates.
                                         No unattended mode.

DRM/KMS (our approach): App → drmModeGetFB2() → DMA-BUF → pixels
                                                   ↑
                                            Kernel-level, compositor-agnostic.
                                            Controlled by admin (CAP_SYS_ADMIN).
                                            No user prompt.
```

---

## Project-by-Project Pain

### RustDesk (80K ⭐) — 4 years of Wayland pain

**Timeline of the disaster:**

| Year | What happened |
|---|---|
| 2022 | Issue #670: "unsupported display server type wayland." Official answer: "switch to X11" |
| 2023 | v1.2.3: first PipeWire attempts. Worked on GNOME, broke on KDE |
| 2024 | Issue #8600: "Failed to obtain screen capture" on KDE Plasma 6. 28 comments. OBS worked, RustDesk didn't |
| 2025 | PR #13537: cursor misalignment. PR #12900: multi-display refactor. Issue #13705: mouse stopped working after update (24 comments) |
| 2026 | PR #14384: KDE Dim Screen kills session → must restart app. Issue #12293: keyboard transmits keys wrong. **Still open** |

**Why they haven't used DRM/KMS**: Locked into PipeWire model. Single developer (`fufesou`) does 90% of Wayland work. No bandwidth to rethink architecture. No embeddable C library exists to make it easy (our niche).

**What they'd need from libdrmtap**: C API → Rust `-sys` crate, privileged helper already solved, multi-GPU/multi-monitor, no compositor dependency.

---

### Sunshine (22K ⭐) — KMS capture works but painfully

**Active issues:**
- KMS capture fails on Flatpak installs (can't setcap inside sandbox)
- "DRM Framebuffer: Probably not permitted" — permissions problems
- Black screens on Sway/wlroots with Vulkan hosts
- Missing mouse cursors during KMS capture
- Corrupt output on Intel Arc (aux planes / DRM modifiers)
- No PipeWire window capture — relies entirely on KMS

**What they'd gain from libdrmtap**: Replace their internal `kms_cap.cpp` with a maintained, tested library. Stop reinventing GPU detection, tiling detection, and permission handling.

---

### wayvnc (1.8K ⭐) — Only works on wlroots

wayvnc uses `wlr-screencopy-unstable-v1`, which is a wlroots-specific Wayland protocol. It **doesn't work on GNOME or KDE** at all. The new Wayland capture protocols (`ext-image-capture-source-v1`) are not yet widely deployed.

**What they'd gain from libdrmtap**: Universal capture that works on ANY compositor — GNOME, KDE, Sway, Cosmic, etc.

---

### GNOME Remote Desktop — Headless problems

GNOME's built-in RDP/VNC server uses Mutter's screencast API + PipeWire. Issues:
- Headless mode (no monitor) frequently produces black screens
- Non-persistent sessions: disconnecting logs out the user
- Requires HDMI dummy dongles for headless servers
- PipeWire capture freezes on unredirected windows (fixed in GNOME 46)

**What they'd gain from libdrmtap**: Fallback capture when PipeWire fails. Reliable headless capture without dummy dongles.

---

### OBS Studio (63K ⭐) — PipeWire is the only option

OBS on Wayland uses PipeWire exclusively. No direct framebuffer access. When PipeWire breaks (which happens on updates), screen recording stops working entirely.

**What they'd gain from libdrmtap**: Alternative capture backend. Like how OBS has X11 capture, PipeWire capture, and could have DRM/KMS capture.

---

### FFmpeg kmsgrab — Works but not embeddable

FFmpeg has `kmsgrab` which does DRM/KMS capture correctly. But you can't embed FFmpeg into a small application — it's 40MB+ of dependencies. And `kmsgrab` requires `CAP_SYS_ADMIN` on the FFmpeg binary itself, with no helper pattern.

**What libdrmtap offers that kmsgrab doesn't**: Embeddable C library (~100KB), automatic helper binary, GPU-specific pipeline selection, clean API.

---

## Common Failure Patterns Across All Projects

| Problem | Affected projects | Root cause |
|---|---|---|
| "Black screen" | RustDesk, Sunshine, GNOME RDP | PipeWire/portal permission failure |
| "Works on GNOME, breaks on KDE" | RustDesk, OBS | Different xdg-desktop-portal backends |
| "Requires user to click allow" | All PipeWire-based | Wayland security model — by design |
| "Doesn't work headless" | GNOME RDP, Sunshine | No user to grant permission |
| "Cursor misalignment" | RustDesk, Sunshine | Multi-monitor coordinate translation |
| "Permissions error" | Sunshine, kmsvnc | CAP_SYS_ADMIN not configured |
| "Breaks after distro update" | RustDesk, OBS | PipeWire API changes |

**All of these problems disappear with DRM/KMS capture** because:
- No PipeWire, no portal, no user prompt
- Works identically on GNOME, KDE, Sway, Cosmic, or any compositor
- Works headless (DRM doesn't need a display server)
- Cursor handled via DRM cursor planes (kernel-level)
- Immune to compositor updates

---

## The Average User's Pain

Typical experience trying remote desktop on Ubuntu Wayland:

1. **Install RustDesk** → prompted to select screen (system dialog)
2. **Select screen** → sometimes works, sometimes black screen
3. **Restart** → prompted to select again (doesn't remember selection)
4. **On KDE**: Dim Screen kills session → must restart app
5. **Multi-monitor**: mouse doesn't work on one or the other
6. **On Ubuntu**: sometimes breaks PipeWire dependencies during install/uninstall
7. **Community recommended solution**: "switch to X11"

This is what RustDesk, Sunshine, and everyone else lives with. **libdrmtap exists because this shouldn't be the state of the art in 2026.**

---

## Implications for libdrmtap

> The problem is not project-specific — it's ecosystem-wide. Every project that captures screens on Linux Wayland is reinventing the same broken wheel. libdrmtap provides the missing infrastructure layer.

**Target integrators** (in priority order):
1. **RustDesk** — most urgent need, largest user base affected
2. **Sunshine** — already uses KMS internally, would simplify their code
3. **VNC servers** — wayvnc, kmsvnc, TigerVNC — all need universal capture
4. **GNOME/KDE RDP** — as a fallback for headless/unattended
5. **CI/testing tools** — screenshot automation on headless servers
