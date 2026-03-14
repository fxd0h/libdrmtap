# libdrmtap-sys

Raw FFI bindings for [libdrmtap](https://github.com/fxd0h/libdrmtap) — a C library for capturing Linux screen framebuffers via DRM/KMS.

## What is libdrmtap?

libdrmtap captures screen contents at the kernel level using DRM/KMS APIs. Unlike PipeWire/portal-based capture, it works:

- **At the login screen** (GDM, SDDM, LightDM)
- **Without user interaction** (no "Select screen to share" prompt)
- **On Wayland** (bypasses the compositor security model)
- **Headless** (no display server needed)

## ⚠️ Testing Status

> **This crate has been tested on `virtio_gpu` (QEMU/Parallels VMs) only.**
>
> Intel (i915/xe), AMD (amdgpu), and Nvidia (nvidia-drm) GPU backends are
> implemented but **not yet validated on real hardware**. The EGL-based
> universal detiling backend compiles and is ready for testing.
>
> If you test on real hardware, please report results via
> [GitHub Issues](https://github.com/fxd0h/libdrmtap/issues).

## Requirements

- Linux with DRM/KMS support
- `libdrmtap` installed (`meson install -C build`)
- `pkg-config` to locate the library

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
