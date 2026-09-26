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
- **`grab_desc()`** (since 0.5.7) — the same zero-copy grab plus a `DmabufDesc`: the plane layout
  (`num_planes`/`offsets`/`pitches`) and HDR state that a `Frame` does not carry. Without them a
  compressed (Intel CCS) or HDR scanout cannot be imported at all, because you hold the fd and no
  way to know where the planes sit inside it. The descriptor deliberately carries **no file
  descriptor**: it is metadata only, so it is safe to serialize, and the fd travels out of band over
  `SCM_RIGHTS` as the C header prescribes
- **`Frame::dma_buf_borrowed_fd()`** (since 0.5.7) — the fd as a `BorrowedFd` tied to the frame,
  `None` on the mapped paths where there is none. **`Frame::try_clone_fd()`** returns an `OwnedFd`
  for the case where it must outlive the frame; it dups, because the frame closes its own on drop.
  The older `dma_buf_fd() -> i32` is deprecated: an integer expresses neither the ownership nor the
  lifetime, and `-1` in it means "no DMA-BUF" rather than being a descriptor
- **`grab_mapped()`** — mmap'd pixel data (for software access)
- **`get_cursor()`** — cursor plane position (top-left of the image, in the CRTC's physical pixels) + ARGB image. On bare-metal drivers the hotspot reads `0`, and the `Cursor::hot_x` documentation gives the two ways to recover one. **`Cursor::hotspot_from_driver()`** (since 0.5.6) says whether that `0` is the driver's own answer: `Some(true)` means both `HOTSPOT_X` and `HOTSPOT_Y` were read, so `hot_x`/`hot_y` are the driver's coordinates even at `(0, 0)`; `Some(false)` means **at least one** was absent, so they carry no information and a hotspot has to be estimated; `None` means nothing recorded an answer for that sample, which is not the same as `Some(false)`
- **`list_displays()`** — enumerate connected monitors
- **`displays_changed()`** — hotplug detection
- **`plane_rotation()`** (since 0.5.8): the DRM `rotation` bitmask the primary plane scans out with, read now, for the plane the last grab read from. `Some(0x1)` is rotate-0, `Some(0x4)` rotate-180 and so on (a reflection adds `0x10`/`0x20`); `None` means the library cannot say (no plane bound, or the property set could not be read). A plane WITHOUT the property answers `Some(0x1)`: it cannot have turned the scanout, so the frame arrives turned by the whole output transform and has to be turned back by it; a frame from a plane that rotated or reflected is already upright and is left alone. Measured: mutter on i915 turns 180 in hardware (`0x4`, the scanout is upright); KWin on amdgpu turns in software (`0x1`, the scanout is upside down).
- **`Error::io_error()`** (since 0.5.7) — the error as an `io::Error` when it really is an errno.
  Not every negative return is one: `drmtap_drm_fd()` uses a bare `-1` as a sentinel, so that value
  answers `None` rather than being rendered as `EPERM`, an error nothing reported. The `code` field
  is unchanged

## Optional features

- **`drm-fourcc`** — adds `Frame::drm_format()` and `DmabufDesc::drm_format()`, returning
  `drm_fourcc::DrmFormat`. Off by default on purpose: a third-party type in a public signature ties
  this crate's semver to that crate's, permanently, and this wrapper sits under a capture library
  other people pin. `format()` stays a `u32` for everyone, and `Frame`'s `Debug` already prints the
  readable fourcc (`XR24`) with no dependency at all

## Requirements

- Rust 1.66 or newer (`std::os::fd`, which the frame's descriptor accessors are built on). Declared
  as `rust-version`, so an older toolchain says so instead of failing on a type
- Linux with DRM/KMS (kernel 4.20+ for the tiled/modifier path; linear/VM
  framebuffers work on older kernels)
- A C compiler and the development packages `libdrmtap-sys` builds against. On
  Debian/Ubuntu: `libdrm-dev libegl-dev libgles2-mesa-dev libseccomp-dev
  libcap-dev`. libdrm, libseccomp and libcap are linked.
  libEGL and libGLESv2 are not: the library dlopens `libEGL.so.1` and
  `libGLESv2.so.2` on first use, so the build needs their headers and **the
  target needs those runtime libraries** (`libegl1` and `libgles2` on
  Debian/Ubuntu). Without them the EGL detile is unavailable and only the CPU
  paths remain, which do not cover every scanout. The crate
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
