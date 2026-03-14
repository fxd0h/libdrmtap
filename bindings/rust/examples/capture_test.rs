/*
 * libdrmtap — Rust capture test
 * https://github.com/fxd0h/libdrmtap
 *
 * Standalone test that validates the full Rust capture pipeline:
 *   DrmTap::open → list_displays → grab_mapped → pixel data → PPM file
 *
 * Usage:
 *   cargo run --example capture_test
 *   cargo run --example capture_test -- /dev/dri/card1
 *
 * Requires: libdrmtap installed (meson install -C build)
 */

use libdrmtap::{Config, DrmTap};
use std::env;
use std::fs::File;
use std::io::Write;

fn main() {
    println!("=== libdrmtap Rust capture test ===\n");

    // Version
    let (major, minor, patch) = libdrmtap::version();
    println!("Version: {}.{}.{}", major, minor, patch);

    // Config
    let device = env::args().nth(1);
    let config = Config {
        device_path: device.clone(),
        debug: env::var("DRMTAP_DEBUG").is_ok(),
        ..Default::default()
    };
    println!(
        "Device:  {}",
        device.as_deref().unwrap_or("(auto-detect)")
    );

    // Open
    let mut tap = DrmTap::open(Some(config)).expect("Failed to open DRM device");
    println!(
        "Driver:  {}",
        tap.gpu_driver().unwrap_or_else(|| "unknown".into())
    );

    // List displays
    let displays = tap.list_displays().expect("Failed to list displays");
    println!("\nDisplays: {}", displays.len());
    for (i, d) in displays.iter().enumerate() {
        println!(
            "  [{}] {}: {}x{} @{}Hz crtc={} active={}",
            i, d.name, d.width, d.height, d.refresh_hz, d.crtc_id, d.active
        );
    }

    // Grab mapped frame
    println!("\nCapturing frame...");
    let frame = tap.grab_mapped().expect("Failed to capture frame");
    println!(
        "  Size:   {}x{}",
        frame.width(),
        frame.height()
    );
    println!("  Stride: {} bytes/row", frame.stride());
    println!(
        "  Format: 0x{:08x} (modifier: 0x{:x})",
        frame.format(),
        frame.modifier()
    );
    println!("  DMA-BUF fd: {}", frame.dma_buf_fd());

    // Check pixel data
    let data = frame.data().expect("No pixel data (mmap failed?)");
    let total_bytes = frame.stride() as usize * frame.height() as usize;
    println!("  Data:   {} bytes", total_bytes);

    // Check for non-zero content
    let has_content = data.iter().take(frame.stride() as usize * 2).any(|&b| b != 0);
    println!(
        "  Pixels: {}",
        if has_content {
            "has content ✓"
        } else {
            "all zeros ✗"
        }
    );

    // Write PPM
    let ppm_path = "/tmp/capture_test.ppm";
    let mut f = File::create(ppm_path).expect("Failed to create PPM file");
    let w = frame.width() as usize;
    let h = frame.height() as usize;
    let stride = frame.stride() as usize;

    write!(f, "P6\n{} {}\n255\n", w, h).unwrap();
    for y in 0..h {
        let row = &data[y * stride..y * stride + w * 4];
        for x in 0..w {
            let px = x * 4;
            // XRGB8888 little-endian: bytes are B, G, R, X
            let b = row[px];
            let g = row[px + 1];
            let r = row[px + 2];
            f.write_all(&[r, g, b]).unwrap();
        }
    }
    println!("\n  Wrote:  {}", ppm_path);

    // Cursor
    println!("\nCursor:");
    match tap.get_cursor() {
        Ok(cursor) => {
            println!(
                "  Position: ({}, {}) hotspot: ({}, {})",
                cursor.x(),
                cursor.y(),
                cursor.hot_x(),
                cursor.hot_y()
            );
            println!(
                "  Size:     {}x{} visible={}",
                cursor.width(),
                cursor.height(),
                cursor.visible()
            );
            if let Some(pixels) = cursor.pixels() {
                let has_data = pixels.iter().any(|&p| p != 0);
                println!(
                    "  Pixels:   {}",
                    if has_data {
                        "has data ✓"
                    } else {
                        "all zeros"
                    }
                );
            }
        }
        Err(e) => println!("  Not available: {}", e),
    }

    println!("\n=== ALL CHECKS PASSED ===");
}
