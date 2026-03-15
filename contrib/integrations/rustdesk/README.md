# RustDesk Integration

Drop-in DRM/KMS capture backend for [RustDesk](https://github.com/rustdesk/rustdesk).

## Status

**Tested and verified** on Ubuntu 24.04 VM (virtio_gpu):
- RustDesk's `screenshot` example captured 1920×1017 desktop via DRM
- Full `cargo build` passes (zero errors, ~50 crate dependencies)
- Binary links `libdrmtap.so.0` successfully

## What It Does

Replaces PipeWire/portal-based capture with direct DRM/KMS framebuffer
capture. This means:

- **No user consent popup** (no "Select the screen to be shared")
- **Works everywhere**: login screen, headless, VMs, kiosks, Wayland
- **No PipeWire dependency**
- **Handles GPU tiling** automatically (Intel, AMD, Nvidia)

## Files

| File | Purpose |
|------|---------|
| `drm/mod.rs` | Module entry point with integration instructions |
| `drm/recorder.rs` | Complete capture backend (inline FFI, self-contained) |

## Quick Integration

Copy `drm/recorder.rs` to `libs/scrap/src/common/drm.rs` in the
RustDesk tree. See `drm/mod.rs` for detailed step-by-step instructions.

## Requirements

- `libdrmtap` installed (headers + shared library)
- Linux with DRM/KMS (virtually all modern Linux systems)
- `CAP_SYS_ADMIN` or the `drmtap-helper` setcap binary for GPU access
