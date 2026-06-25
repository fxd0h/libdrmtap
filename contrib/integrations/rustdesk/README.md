# RustDesk Integration

DRM/KMS capture backend for [RustDesk](https://github.com/rustdesk/rustdesk),
built on the static [`libdrmtap-sys`](https://crates.io/crates/libdrmtap-sys)
crate.

> The canonical integration is the upstream PR
> [rustdesk/rustdesk#15420](https://github.com/rustdesk/rustdesk/pull/15420).
> The files here are a self-contained reference for the same crate-based backend.

## Status

**Tested and verified** on Ubuntu 24.04:
- RustDesk's `screenshot` example captured the desktop via DRM
- Full `cargo build` passes (zero errors)
- Verified on virtio_gpu (QEMU/KVM), Intel Meteor Lake (i915, EGL CCS detiling,
  dual 4K), and NVIDIA Jetson Orin Nano (nvidia-drm, Wayland). AMD (amdgpu) is
  implemented but still untested.

## What It Does

Replaces PipeWire/portal-based capture with direct DRM/KMS framebuffer
capture. This means:

- **No user consent popup** (no "Select the screen to be shared")
- **Works everywhere**: login screen, headless, VMs, kiosks, Wayland
- **No PipeWire dependency**
- **Handles GPU tiling** automatically (Intel i915 and Nvidia verified; AMD
  amdgpu implemented but untested)

## Files

| File | Purpose |
|------|---------|
| `drm/mod.rs` | Module entry point with integration instructions |
| `drm/recorder.rs` | Capture backend (uses the `libdrmtap-sys` crate) |

## Quick Integration

1. Add the crate to `libs/scrap/Cargo.toml`:

   ```toml
   libdrmtap-sys = { version = "0.4.1", optional = true }
   ```

   It embeds and statically compiles the C sources **and** builds the
   `drmtap-helper` binary — no system `libdrmtap` install, no `meson install`,
   no `pkg-config` lookup of a shared library, no `rustc-link-search`.

2. Copy `drm/recorder.rs` to `libs/scrap/src/common/drm.rs` and wire it into the
   `Display`/`Capturer` enums and the `linux.rs` dispatcher (see `drm/mod.rs`).

The upstream PR above has the full, current backend.

## Requirements

- Linux with DRM/KMS (kernel 4.20+ for the tiled/modifier framebuffer path —
  Ubuntu 20.04+; linear/VM framebuffers work on older kernels)
- `CAP_SYS_ADMIN` or the `drmtap-helper` setcap binary for GPU access (the helper
  is built automatically by `libdrmtap-sys`)
