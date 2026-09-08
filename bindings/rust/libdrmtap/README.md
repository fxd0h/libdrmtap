# libdrmtap

Safe Rust wrapper for [libdrmtap](https://github.com/fxd0h/libdrmtap) — DRM/KMS screen capture for Linux.

Capture the screen at the kernel level: login screens, Wayland, headless — no user prompts.

Frames come back as 8-bit BGRA. Tiled/compressed framebuffers are GPU-detiled,
and **HDR10** scanouts (PQ/BT.2020) are tone-mapped to SDR when the connector
reports HDR (`P010` overlay-video and HLG excepted).

## ⚠️ Testing Status

> **Verified here:** Intel Meteor Lake-P (`i915`, multi-display 4K, EGL
> detiling), AMD RX560 (Polaris/gfx8), NVIDIA Jetson Orin Nano (`nvidia-drm`,
> aarch64, Wayland), and `virtio_gpu` (QEMU/Parallels VMs).
>
> **Confirmed by outside testers, on their hardware:** AMD RX Vega 64 (gfx9, X11)
> by GK-Gaming, and Intel Raptor Lake with a hybrid NVIDIA GPU (Ubuntu 26.04,
> GNOME Wayland) by huzhifeng. One host each, not a support matrix.
>
> All of it depends on the EGL detile backend, which `libdrmtap-sys` always
> compiles in. It needs the EGL and GLES2 headers at build time (`libegl-dev`
> and `libgles2-mesa-dev` on Debian/Ubuntu); `cargo build` fails without them
> rather than quietly producing a CPU-only library. Hit a problem on other
> hardware? Please [report results](https://github.com/fxd0h/libdrmtap/issues).

## Installation

```toml
[dependencies]
libdrmtap = "0.5"
```

This pulls in `libdrmtap-sys`, which embeds and statically compiles the C
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
- **`get_cursor()`** — cursor plane position (top-left of the image, in the CRTC's physical pixels) + ARGB image. The hotspot comes back `0` on bare-metal drivers; see `Cursor::hot_x` for how to recover it
- **`list_displays()`** — enumerate connected monitors
- **`displays_changed()`** — hotplug detection

## Requirements

- Linux with DRM/KMS (kernel 4.20+ for the tiled/modifier path; linear/VM
  framebuffers work on older kernels)
- A C compiler and the development packages `libdrmtap-sys` builds against. On
  Debian/Ubuntu: `libdrm-dev libegl-dev libgles2-mesa-dev libseccomp-dev
  libcap-dev`. libdrm, libseccomp and libcap are linked; libEGL and libGLESv2 are
  needed only as headers, because the library dlopens them at runtime. The crate
  compiles its embedded C sources statically, so there is no system `libdrmtap`
  install required
- For unprivileged capture: `drmtap-helper`, which `libdrmtap-sys` always builds
  (the meson `-Dhelper=disabled` option that drops it has no crate equivalent). A
  file capability applies to every user who can `exec` the binary, so restrict who
  can run it FIRST (`root:<capture-group>`, mode `0750`) and apply
  `cap_sys_admin+ep` LAST. Procedure in
  [SECURITY.md](https://github.com/fxd0h/libdrmtap/blob/main/SECURITY.md)

## License

MIT
