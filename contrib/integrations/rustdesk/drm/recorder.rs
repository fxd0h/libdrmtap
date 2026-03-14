// DRM frame capture recorder for RustDesk
//
// Implements the `Recorder` trait from RustDesk's wayland::capturable module.
// Each call to `capture()` grabs the current framebuffer via libdrmtap
// and returns it as BGR0S pixel data.

use super::super::wayland::capturable::PixelProvider;
use super::super::wayland::capturable::Recorder;
use libdrmtap::{Config, DrmTap};
use std::error::Error;

/// DRM framebuffer recorder using libdrmtap.
///
/// Captures frames directly from the kernel DRM/KMS subsystem,
/// bypassing the Wayland compositor entirely. Requires either
/// root/CAP_SYS_ADMIN or the drmtap-helper binary with setcap.
pub struct DrmRecorder {
    tap: DrmTap,
    capture_cursor: bool,
    /// Reusable buffer — allocated once, reused across frames
    buffer: Vec<u8>,
    /// Cached frame dimensions for PixelProvider
    last_width: usize,
    last_height: usize,
    last_stride: usize,
}

impl DrmRecorder {
    /// Create a new DRM recorder for a specific CRTC.
    ///
    /// If `crtc_id` is 0, auto-selects the primary display.
    pub fn new(crtc_id: u32, capture_cursor: bool) -> Result<Self, Box<dyn Error>> {
        let config = Config {
            crtc_id,
            debug: std::env::var("DRMTAP_DEBUG").is_ok(),
            ..Default::default()
        };

        let tap = DrmTap::open(Some(config))?;

        Ok(DrmRecorder {
            tap,
            capture_cursor,
            buffer: Vec::new(),
            last_width: 0,
            last_height: 0,
            last_stride: 0,
        })
    }
}

impl Recorder for DrmRecorder {
    fn capture(&mut self, _timeout_ms: u64) -> Result<PixelProvider, Box<dyn Error>> {
        // Grab a frame with mapped pixel data
        let frame = self.tap.grab_mapped()?;

        // Get pixel data as a byte slice
        let data = match frame.data() {
            Some(d) => d,
            None => return Ok(PixelProvider::NONE),
        };

        let width = frame.width() as usize;
        let height = frame.height() as usize;
        let stride = frame.stride() as usize;
        let frame_size = stride * height;

        // Reuse buffer — only reallocate if frame size changed
        if self.buffer.len() != frame_size {
            self.buffer.resize(frame_size, 0);
        }

        // Copy pixel data into our reusable buffer.
        // We must copy because the Frame's mmap is released on drop.
        self.buffer[..frame_size].copy_from_slice(&data[..frame_size]);
        self.last_width = width;
        self.last_height = height;
        self.last_stride = stride;

        // DRM gives us XRGB8888 in little-endian = B, G, R, X per pixel
        // This maps to RustDesk's BGR0S (BGRA with stride)
        //
        // Safety: We return a reference to self.buffer which lives as
        // long as this DrmRecorder. RustDesk's capture pipeline copies
        // the data into the encoder before the next capture() call.
        let data_ref: &[u8] = unsafe {
            std::slice::from_raw_parts(self.buffer.as_ptr(), frame_size)
        };

        Ok(PixelProvider::BGR0S(width, height, stride, data_ref))
    }
}

// Note on cursor capture:
//
// When capture_cursor is true, we could overlay the cursor onto the frame
// using drmtap_get_cursor(). However, RustDesk typically sends cursor
// position + shape separately for client-side rendering (lower latency).
//
// A future enhancement could add a DrmCursorRecorder that provides
// cursor data through RustDesk's cursor channel instead.

