# libdrmtap

Safe Rust wrapper for [libdrmtap](https://github.com/fxd0h/libdrmtap) — DRM/KMS screen capture for Linux.

Capture the screen at the kernel level: login screens, Wayland, headless — no user prompts.

Frames come back as 8-bit BGRA. Tiled/compressed framebuffers are GPU-detiled,
and **HDR10** scanouts (PQ/BT.2020) are tone-mapped to SDR when the connector
reports HDR (`P010` overlay-video and HLG excepted).

## ⚠️ Testing Status

> **Verified on `virtio_gpu` (QEMU/Parallels VMs), Intel Meteor Lake (`i915`,
> dual 4K, EGL CCS detiling), and NVIDIA Jetson Orin Nano (`nvidia-drm`,
> Wayland).**
>
> The AMD (`amdgpu`) backend is validated on real hardware (RX Vega 64, gfx9,
> via EGL detile). Hit a problem on other AMD hardware? Please
> [report results](https://github.com/fxd0h/libdrmtap/issues).

## Installation

```toml
[dependencies]
libdrmtap = "0.3"
```

This pulls in `libdrmtap-sys` 0.4.9, which embeds and statically compiles the C
sources (and the privilege helper) — no system `libdrmtap` install needed.

## Example

```rust
use libdrmtap::DrmTap;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut tap = DrmTap::open(None)?;

    println!("GPU: {}", tap.gpu_driver().unwrap_or("unknown".into()));

    // List displays
    for d in tap.list_displays()? {
        println!("{}: {}x{}@{}Hz", d.name, d.width, d.height, d.refresh_hz);
    }

    // Capture a frame with mapped pixel data
    let frame = tap.grab_mapped()?;
    println!("Captured: {}x{} stride={}", frame.width(), frame.height(), frame.stride());

    if let Some(pixels) = frame.data() {
        println!("First pixel (BGR): {:02x}{:02x}{:02x}",
                 pixels[0], pixels[1], pixels[2]);
    }

    Ok(())
}
```

## Features

- **`DrmTap::open()`** — auto-detect GPU and display
- **`grab()`** — zero-copy DMA-BUF fd (for hardware encoders)
- **`grab_mapped()`** — mmap'd pixel data (for software access)
- **`get_cursor()`** — cursor position + ARGB image
- **`list_displays()`** — enumerate connected monitors
- **`displays_changed()`** — hotplug detection

## Requirements

- Linux with DRM/KMS (kernel 4.20+ for the tiled/modifier path; linear/VM
  framebuffers work on older kernels)
- A C compiler — `libdrmtap-sys` compiles its embedded C sources statically, so
  there is no system `libdrmtap` install required
- For unprivileged capture: `drmtap-helper` with `cap_sys_admin+ep` (the helper
  binary is built by `libdrmtap-sys`)

## License

MIT
