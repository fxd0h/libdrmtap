// DRM capturable implementation for RustDesk
//
// Implements the `Capturable` trait, acting as a factory for DrmRecorder.
// Each DrmCapturable represents a DRM display that can be captured.

use super::display::DrmDisplayInfo;
use super::recorder::DrmRecorder;
use super::super::wayland::capturable::{BoxCloneCapturable, Capturable, Recorder};
use std::error::Error;

/// A DRM display that can be captured.
///
/// Implements RustDesk's `Capturable` trait, providing display info
/// and creating `DrmRecorder` instances for frame capture.
#[derive(Clone)]
pub struct DrmCapturable {
    pub display: DrmDisplayInfo,
}

impl DrmCapturable {
    pub fn new(display: DrmDisplayInfo) -> Self {
        DrmCapturable { display }
    }
}

impl Capturable for DrmCapturable {
    fn name(&self) -> String {
        format!(
            "{} ({}x{} DRM)",
            self.display.name, self.display.width, self.display.height
        )
    }

    fn geometry_relative(&self) -> Result<(f64, f64, f64, f64), Box<dyn Error>> {
        // DRM captures the full display, so geometry is always the full screen
        // For multi-monitor, position would need compositor info
        Ok((0.0, 0.0, 1.0, 1.0))
    }

    fn before_input(&mut self) -> Result<(), Box<dyn Error>> {
        // DRM capture doesn't need to focus anything — we capture
        // the framebuffer directly, not a window
        Ok(())
    }

    fn recorder(&self, capture_cursor: bool) -> Result<Box<dyn Recorder>, Box<dyn Error>> {
        let recorder = DrmRecorder::new(self.display.crtc_id, capture_cursor)?;
        Ok(Box::new(recorder))
    }
}

impl BoxCloneCapturable for DrmCapturable {
    fn box_clone(&self) -> Box<dyn Capturable> {
        Box::new(self.clone())
    }
}

/// Get all DRM capturables (one per active display).
pub fn get_drm_capturables() -> Vec<Box<dyn Capturable>> {
    super::display::get_drm_displays()
        .into_iter()
        .map(|d| Box::new(DrmCapturable::new(d)) as Box<dyn Capturable>)
        .collect()
}
