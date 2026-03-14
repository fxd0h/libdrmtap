# libdrmtap

Safe Rust wrapper for [libdrmtap](https://github.com/fxd0h/libdrmtap) — DRM/KMS screen capture for Linux.

Capture the screen at the kernel level: login screens, Wayland, headless — no user prompts.

## ⚠️ Testing Status

> **Tested on `virtio_gpu` (QEMU/Parallels VMs) only.**
>
> Intel, AMD, and Nvidia backends are implemented but not yet validated
> on real hardware. If you test on real GPUs, please
> [report results](https://github.com/fxd0h/libdrmtap/issues).

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

- Linux with DRM/KMS
- `libdrmtap` C library installed
- For unprivileged capture: `drmtap-helper` with `cap_sys_admin+ep`

## License

MIT
