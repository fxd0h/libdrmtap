# libdrmtap-sys

Raw FFI bindings for [libdrmtap](https://github.com/fxd0h/libdrmtap) — a C library for capturing Linux screen framebuffers via DRM/KMS.

## What is libdrmtap?

libdrmtap captures screen contents at the kernel level using DRM/KMS APIs. Unlike PipeWire/portal-based capture, it works:

- **At the login screen** (GDM, SDDM, LightDM)
- **Without user interaction** (no "Select screen to share" prompt)
- **On Wayland** (bypasses the compositor security model)
- **Headless** (no display server needed)

## ⚠️ Testing Status

> **Verified working:** `virtio_gpu` (QEMU/Parallels VMs), Intel Meteor Lake
> (`i915`, dual 3840x2160, EGL CCS detiling of the tiled/compressed framebuffer),
> and NVIDIA Jetson Orin Nano (`nvidia-drm`, Wayland).
>
> The AMD (`amdgpu`) backend is implemented but **not yet validated on real
> hardware**.
>
> If you test AMD or other configurations, please report results via
> [GitHub Issues](https://github.com/fxd0h/libdrmtap/issues).

## Requirements

- Linux with DRM/KMS support (kernel 4.20+ for the tiled/modifier framebuffer
  path, i.e. Ubuntu 20.04+; linear/VM framebuffers work on older kernels)
- A C compiler — the embedded C sources are compiled statically at build time.
  There is **no** system `libdrmtap` install, `meson install`, or `pkg-config`
  lookup of a shared library.
- `libdrm` development headers (located via `pkg-config`), plus EGL, OpenGL ES 2,
  libseccomp, and libcap (and their `-dev` headers) — the crate links these.

## Usage

This is a `-sys` crate with raw FFI bindings. For a safe wrapper, use [`libdrmtap`](https://crates.io/crates/libdrmtap).

```rust
use libdrmtap_sys::*;
use std::ptr;

unsafe {
    let ctx = drmtap_open(ptr::null());
    if !ctx.is_null() {
        let driver = drmtap_gpu_driver(ctx);
        // ... capture frames ...
        drmtap_close(ctx);
    }
}
```

## License

MIT
