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
// Integration (tested on Ubuntu 24.04 with virtio_gpu):
//
//   1. Install libdrmtap:
//        sudo apt install libdrmtap-dev
//      Or build from source:
//        meson setup build && meson compile -C build && sudo meson install -C build
//
//   2. Copy this directory to: libs/scrap/src/common/drm.rs
//      (Just the recorder.rs content — it's a single self-contained file)
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
//   5. In libs/scrap/build.rs (inside the unix branch):
//        println!("cargo:rustc-link-search=/usr/local/lib/x86_64-linux-gnu");
//
//   6. Build:
//        cargo build
//
// The recorder.rs file contains the complete backend implementation.

pub mod recorder;
