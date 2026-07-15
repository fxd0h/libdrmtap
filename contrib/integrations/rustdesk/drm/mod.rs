// RustDesk DRM/KMS capture backend — powered by libdrmtap
//
// This module replaces PipeWire/portal-based capture in RustDesk's `scrap`
// crate with direct DRM/KMS framebuffer capture via libdrmtap.
//
// Advantages over PipeWire/portal:
//   - No user consent popup ("Select the screen to be shared")
//   - Works on login screen, headless, VMs (virtio_gpu, QEMU, Parallels)
//   - No PipeWire/GStreamer dependency
//   - Works on kiosk and embedded systems
//   - Handles GPU tiling (Intel, AMD, Nvidia) automatically
//
// Integration (tested on Ubuntu 24.04; uses the static libdrmtap-sys crate):
//
//   1. Add the crate to libs/scrap/Cargo.toml:
//        libdrmtap-sys = { version = "0.4.8", optional = true }
//      It statically embeds and compiles the C sources and builds the
//      drmtap-helper binary — no system libdrmtap install, no meson install,
//      no apt package, and no rustc-link-search.
//
//   2. Copy recorder.rs to: libs/scrap/src/common/drm.rs
//      (a single self-contained file)
//
//   3. In libs/scrap/src/common/mod.rs, inside the cfg_if! block:
//        mod drm;
//
//   4. In libs/scrap/src/common/linux.rs:
//        - Add `DRM(drm::Display)` to the Display enum
//        - Add `DRM(drm::Capturer)` to the Capturer enum
//        - In Display::all(), try drm::Display::all() first
//        - Add DRM arms to all match blocks
//
//   5. Build:
//        cargo build --features drm
//
// The recorder.rs file contains the backend implementation. The canonical,
// current version is in the upstream PR rustdesk/rustdesk#15420.

pub mod recorder;
