// DRM display enumeration for RustDesk
//
// Uses libdrmtap to enumerate DRM displays, mapped to RustDesk's
// WaylandDisplayInfo format for compatibility with the existing UI.

use libdrmtap::{Config, DrmTap};
use std::error::Error;

/// Display info matching RustDesk's WaylandDisplayInfo structure
#[derive(Debug, Clone)]
pub struct DrmDisplayInfo {
    pub name: String,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub refresh_hz: u32,
    pub crtc_id: u32,
    pub active: bool,
}

/// Enumerate DRM displays using libdrmtap.
///
/// Returns a list of active displays. Falls back to empty list on error.
pub fn get_drm_displays() -> Vec<DrmDisplayInfo> {
    let config = Config {
        debug: std::env::var("DRMTAP_DEBUG").is_ok(),
        ..Default::default()
    };

    let mut tap = match DrmTap::open(Some(config)) {
        Ok(t) => t,
        Err(e) => {
            tracing::warn!("libdrmtap: failed to open: {}", e);
            return Vec::new();
        }
    };

    let displays = match tap.list_displays() {
        Ok(d) => d,
        Err(e) => {
            tracing::warn!("libdrmtap: failed to list displays: {}", e);
            return Vec::new();
        }
    };

    displays
        .into_iter()
        .filter(|d| d.active)
        .map(|d| DrmDisplayInfo {
            name: d.name,
            x: 0, // DRM doesn't provide position — compositor does
            y: 0,
            width: d.width,
            height: d.height,
            refresh_hz: d.refresh_hz,
            crtc_id: d.crtc_id,
            active: d.active,
        })
        .collect()
}

/// Check if DRM capture is available on this system.
pub fn is_drm_available() -> bool {
    DrmTap::open(None).is_ok()
}
