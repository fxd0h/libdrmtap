// RustDesk DRM/KMS capture backend — powered by libdrmtap
//
// This module adds a new capture backend to RustDesk's `scrap` crate
// that uses DRM/KMS direct framebuffer capture via libdrmtap.
//
// Advantages over PipeWire/portal:
// - No user consent popup (no "Select the screen to be shared")
// - Works on VMs (virtio_gpu, Parallels, QEMU)
// - No PipeWire dependency
// - Works on headless/kiosk systems
//
// Usage:
//   Place this directory at libs/scrap/src/drm/ in the RustDesk tree.
//   Add `#[cfg(feature = "drm")] pub mod drm;` to libs/scrap/src/lib.rs
//   Add `libdrmtap = { version = "0.1", optional = true }` to Cargo.toml

pub mod capturable;
pub mod display;
pub mod recorder;

pub use capturable::DrmCapturable;
pub use display::get_drm_displays;
pub use recorder::DrmRecorder;
