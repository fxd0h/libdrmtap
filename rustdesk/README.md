# RustDesk DRM/KMS Backend — Integration Guide

This directory contains a ready-to-use DRM/KMS capture backend for RustDesk's `scrap` crate, powered by **libdrmtap**.

## What This Does

Adds a new capture path to RustDesk that bypasses the Wayland compositor entirely, capturing frames directly from the kernel DRM/KMS subsystem. This solves:

1. **No "Select the screen to be shared" prompt** — unattended remote access works
2. **No PipeWire dependency** — works on minimal systems and VMs
3. **Verified at 57 FPS** on Parallels VM with virtio_gpu

## Files

| File | Purpose |
|---|---|
| `drm/mod.rs` | Module root |
| `drm/recorder.rs` | `DrmRecorder` — implements `Recorder` trait, captures frames |
| `drm/capturable.rs` | `DrmCapturable` — implements `Capturable` trait, per-display factory |
| `drm/display.rs` | Display enumeration via `drmtap_list_displays` |

## Integration Steps

### 1. Install libdrmtap on the build machine

```bash
git clone https://github.com/fxd0h/libdrmtap.git
cd libdrmtap
meson setup build && meson compile -C build
sudo meson install -C build
```

### 2. Copy the `drm/` directory

```bash
cp -r rustdesk/drm/ /path/to/rustdesk/libs/scrap/src/drm/
```

### 3. Add to `libs/scrap/src/lib.rs`

```rust
#[cfg(feature = "drm")]
pub mod drm;
```

### 4. Add to `libs/scrap/Cargo.toml`

```toml
[dependencies]
libdrmtap = { version = "0.1", optional = true }

[features]
drm = ["libdrmtap"]
```

### 5. Wire into capture selection

In the main capture loop (e.g., `src/server/video_service.rs`), add DRM as a fallback:

```rust
#[cfg(feature = "drm")]
{
    if scrap::drm::display::is_drm_available() {
        // Use DRM backend — no user prompt needed
        let capturables = scrap::drm::capturable::get_drm_capturables();
        // ...
    }
}
```

### 6. Set up the helper on the target machine

```bash
sudo setcap cap_sys_admin+ep /usr/local/libexec/drmtap-helper
```

## Requirements

- libdrmtap ≥ 0.1.0 (installed via meson)
- CAP_SYS_ADMIN (via helper binary or root)
- Linux with DRM/KMS (any GPU: Intel, AMD, Nvidia, VMs)
