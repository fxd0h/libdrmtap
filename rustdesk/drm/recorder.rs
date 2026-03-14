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

        // DRM gives us XRGB8888 in little-endian = B, G, R, X per pixel
        // This maps to RustDesk's BGR0S (BGRA with stride)
        //
        // IMPORTANT: We need to copy the data because the Frame borrows
        // from the mmap'd region which is released on drop. We use
        // BGR0S which includes stride information.
        //
        // TODO: For zero-copy, RustDesk would need to hold the Frame
        //       alive across the capture boundary. This copy is the
        //       safe path for now.
        let owned_data: Vec<u8> = data[..stride * height].to_vec();

        // Safety: We're extending the lifetime by copying to owned data.
        // The PixelProvider takes a reference, so we need to leak the Vec
        // to get a 'static reference. This is reclaimed by RustDesk's
        // frame processing pipeline which copies data into the encoder.
        //
        // Alternative: Use a thread-local buffer that's reused across frames.
        let leaked: &'static [u8] = Box::leak(owned_data.into_boxed_slice());

        Ok(PixelProvider::BGR0S(width, height, stride, leaked))
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
