# Potential Adopters and Integration Analysis

> **Date**: 2026-03-14 (updated)  
> **Sources**: GitHub repos, issue trackers, user reports, web research

---

## Tier 1 — Immediate Need (actively struggling with screen capture)

| Project | Stars | Language | Current capture | Pain level | libdrmtap value |
|---|---|---|---|---|---|
| [RustDesk](https://github.com/rustdesk/rustdesk) | ~80K | Rust | PipeWire + xdg-portal | 🔴 Critical | Replace entire Wayland capture layer |
| [Sunshine](https://github.com/LizardByte/Sunshine) | ~22K | C++ | Own KMS + PipeWire | 🔴 Critical | Replace `kms_cap.cpp` with maintained lib |
| [kmsvnc](https://github.com/isjerryxiao/kmsvnc) | ~700 | C | Own DRM/KMS | 🟡 High | Could use libdrmtap as capture backend |
| [wayvnc](https://github.com/any1/wayvnc) | ~1.8K | C | wlr-screencopy (wlroots only) | 🟡 High | Universal capture for all compositors |

### Integration path for each:

**RustDesk**: `cargo add libdrmtap` — both crates published on crates.io (`libdrmtap-sys` + `libdrmtap`). RustDesk adds it as optional backend alongside PipeWire. Priority: `DRM/KMS → PipeWire → X11`. Integration module already written in `contrib/integrations/rustdesk/`.

**Sunshine**: Replace internal KMS code with `#include <drmtap.h>`. They already use DMA-BUF → VAAPI pipeline, so `drmtap_grab()` (zero-copy) slots in directly.

**kmsvnc**: Could become a thin VNC wrapper around libdrmtap. All the capture logic moves to us, they keep the VNC protocol.

**wayvnc**: Add DRM/KMS backend alongside wlr-screencopy. Falls back to libdrmtap when not running under wlroots.

---

## Tier 2 — Significant Benefit

| Project | Stars | Language | How libdrmtap helps |
|---|---|---|---|
| [OBS Studio](https://github.com/obsproject/obs-studio) | ~63K | C/C++ | New "DRM/KMS capture" source plugin alongside PipeWire |
| [FFmpeg](https://ffmpeg.org/) | ~47K | C | Could simplify `kmsgrab` using libdrmtap, or offer as alternative |
| [FreeRDP](https://github.com/FreeRDP/FreeRDP) | ~11K | C | DRM/KMS backend for Linux RDP server |
| [Kodi](https://github.com/xbmc/xbmc) | ~18K | C++ | Screenshot/recording on headless media devices |
| [GNOME Remote Desktop](https://gitlab.gnome.org/GNOME/gnome-remote-desktop) | N/A | C | Fallback when PipeWire/Mutter fails, headless mode |
| [KRdp](https://invent.kde.org/network/krdp) | N/A | C++ | Alternative to KPipeWire for capture |

---

## Tier 3 — Niche but Real Use Cases

| Use case | Who needs it | Why DRM/KMS |
|---|---|---|
| **CI visual testing** | Every CI with Linux GPU runners | Screenshot automation without X11/Wayland |
| **Kiosk monitoring** | Digital signage, ATMs, POS | Remote monitoring without display server |
| **KVM-over-IP** | PiKVM, TinyPilot | Direct framebuffer capture on SBCs |
| **Forensics** | Security auditing tools | Evidence capture on servers |
| **Accessibility** | Screen readers, magnifiers | Read framebuffer content for a11y |
| **Game recording** | Linux gamers | Alternative to PipeWire when it fails |
| **Headless servers** | Server admin tools | No display server, just DRM |

---

## Market Size

```
Projects with capture problems that libdrmtap solves:

  RustDesk     80K ⭐  (Rust, needs -sys crate)
  OBS Studio   63K ⭐  (C/C++, plugin)
  FFmpeg       47K ⭐  (C, already has kmsgrab)
  Sunshine     22K ⭐  (C++, direct integration)
  Kodi         18K ⭐  (C++, plugin)
  FreeRDP      11K ⭐  (C, new backend)
  wayvnc       1.8K ⭐  (C, direct integration)
  kmsvnc       700  ⭐  (C, could rebased on libdrmtap)
                ──────
  Total:      ~243K ⭐  of projects that could integrate
```

---

## What Each Project Needs From Us

| Requirement | RustDesk | Sunshine | VNC servers | OBS | FFmpeg |
|---|---|---|---|---|---|
| `drmtap_grab_mapped()` (RGBA) | ✅ | ❌ | ✅ | ✅ | ❌ |
| `drmtap_grab()` (DMA-BUF fd) | ✅ | ✅ | ❌ | ✅ | ✅ |
| Cursor position + shape | ✅ | ✅ | ✅ | ❌ | ❌ |
| Multi-monitor | ✅ | ✅ | ✅ | ✅ | ✅ |
| HDR metadata | ❌ | ✅ | ❌ | ✅ | ✅ |
| pkg-config integration | ✅ | ✅ | ✅ | ✅ | ✅ |
| Rust `-sys` crate | ✅ | ❌ | ❌ | ❌ | ❌ |
| Rust safe wrapper | ✅ | ❌ | ❌ | ❌ | ❌ |
| Frame timestamps | ❌ | ✅ | ❌ | ✅ | ✅ |

---

## Outreach Strategy

| Step | Action | Status |
|---|---|---|
| 1 | Publish `libdrmtap-sys` + `libdrmtap` on crates.io | ✅ Done |
| 2 | Test on real hardware (Intel, AMD, Nvidia, RPi) | 🔜 Pending hardware access |
| 3 | Post to r/linux, r/rustdesk, r/selfhosted, Hacker News | 🔜 After hardware validation |
| 4 | Open issues on RustDesk and Sunshine repos proposing integration | 🔜 After validation |
| 5 | Create OBS plugin as proof of concept | 🔜 Future |
