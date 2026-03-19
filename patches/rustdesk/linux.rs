//! Linux screen capture backend dispatcher.
//!
//! Selects the best available capture method:
//!   1. DRM/KMS (via libdrmtap) — preferred, works headless, no X11/Wayland
//!   2. X11 (via XShmGetImage) — fallback for X11 sessions
//!   3. Wayland (via PipeWire/portal) — fallback for Wayland sessions

use hbb_common::log;
use crate::{
    common::{
        wayland,
        x11::{self},
        TraitCapturer,
    },
    Frame,
};
use std::{io, time::Duration};

#[cfg(feature = "drm")]
use crate::common::drm;

pub enum Capturer {
    X11(x11::Capturer),
    WAYLAND(wayland::Capturer),
    #[cfg(feature = "drm")]
    DRM(drm::Capturer),
}

impl Capturer {
    pub fn new(display: Display) -> io::Result<Capturer> {
        Ok(match display {
            Display::X11(d) => Capturer::X11(x11::Capturer::new(d)?),
            Display::WAYLAND(d) => Capturer::WAYLAND(wayland::Capturer::new(d)?),
            #[cfg(feature = "drm")]
            Display::DRM(d) => Capturer::DRM(drm::Capturer::new(d)?),
        })
    }

    pub fn width(&self) -> usize {
        match self {
            Capturer::X11(d) => d.width(),
            Capturer::WAYLAND(d) => d.width(),
            #[cfg(feature = "drm")]
            Capturer::DRM(d) => d.width(),
        }
    }

    pub fn height(&self) -> usize {
        match self {
            Capturer::X11(d) => d.height(),
            Capturer::WAYLAND(d) => d.height(),
            #[cfg(feature = "drm")]
            Capturer::DRM(d) => d.height(),
        }
    }
}

impl TraitCapturer for Capturer {
    fn frame<'a>(&'a mut self, timeout: Duration) -> io::Result<Frame<'a>> {
        match self {
            Capturer::X11(d) => d.frame(timeout),
            Capturer::WAYLAND(d) => d.frame(timeout),
            #[cfg(feature = "drm")]
            Capturer::DRM(d) => d.frame(timeout),
        }
    }
}

pub enum Display {
    X11(x11::Display),
    WAYLAND(wayland::Display),
    #[cfg(feature = "drm")]
    DRM(drm::Display),
}

impl Display {
    pub fn primary() -> io::Result<Display> {
        // Try DRM first (bypasses X11/Wayland entirely)
        #[cfg(feature = "drm")]
        {
            match drm::Display::primary() {
                Ok(d) => {
                    log::info!("Using DRM/KMS capture backend");
                    return Ok(Display::DRM(d));
                }
                Err(e) => {
                    log::debug!("DRM not available, falling back: {}", e);
                }
            }
        }

        Ok(if super::is_x11() {
            Display::X11(x11::Display::primary()?)
        } else {
            Display::WAYLAND(wayland::Display::primary()?)
        })
    }

    pub fn all() -> io::Result<Vec<Display>> {
        log::debug!("Linux display enumeration starting");
        // Try DRM first
        #[cfg(feature = "drm")]
        {
            log::debug!("Trying DRM/KMS capture backend");
            match drm::Display::all() {
                Ok(displays) if !displays.is_empty() => {
                    log::info!("DRM: found {} display(s)", displays.len());
                    log::info!("DRM: found {} display(s)", displays.len());
                    return Ok(displays.into_iter().map(Display::DRM).collect());
                }
                Ok(_) => {
                    log::debug!("DRM: no active displays, falling back");
                    log::debug!("DRM: no active displays");
                }
                Err(e) => {
                    log::debug!("DRM enumeration failed, falling back: {}", e);
                    
                }
            }
        }
        log::debug!("Falling back to X11/Wayland");

        Ok(if super::is_x11() {
            x11::Display::all()?
                .drain(..)
                .map(|x| Display::X11(x))
                .collect()
        } else {
            wayland::Display::all()?
                .drain(..)
                .map(|x| Display::WAYLAND(x))
                .collect()
        })
    }

    pub fn width(&self) -> usize {
        match self {
            Display::X11(d) => d.width(),
            Display::WAYLAND(d) => d.width(),
            #[cfg(feature = "drm")]
            Display::DRM(d) => d.width(),
        }
    }

    pub fn height(&self) -> usize {
        match self {
            Display::X11(d) => d.height(),
            Display::WAYLAND(d) => d.height(),
            #[cfg(feature = "drm")]
            Display::DRM(d) => d.height(),
        }
    }

    pub fn scale(&self) -> f64 {
        match self {
            Display::X11(_d) => 1.0,
            Display::WAYLAND(d) => d.scale(),
            #[cfg(feature = "drm")]
            Display::DRM(d) => d.scale(),
        }
    }

    pub fn logical_width(&self) -> usize {
        match self {
            Display::X11(d) => d.width(),
            Display::WAYLAND(d) => d.logical_width(),
            #[cfg(feature = "drm")]
            Display::DRM(d) => d.logical_width(),
        }
    }

    pub fn logical_height(&self) -> usize {
        match self {
            Display::X11(d) => d.height(),
            Display::WAYLAND(d) => d.logical_height(),
            #[cfg(feature = "drm")]
            Display::DRM(d) => d.logical_height(),
        }
    }

    pub fn origin(&self) -> (i32, i32) {
        match self {
            Display::X11(d) => d.origin(),
            Display::WAYLAND(d) => d.origin(),
            #[cfg(feature = "drm")]
            Display::DRM(d) => d.origin(),
        }
    }

    pub fn is_online(&self) -> bool {
        match self {
            Display::X11(d) => d.is_online(),
            Display::WAYLAND(d) => d.is_online(),
            #[cfg(feature = "drm")]
            Display::DRM(d) => d.is_online(),
        }
    }

    pub fn is_primary(&self) -> bool {
        match self {
            Display::X11(d) => d.is_primary(),
            Display::WAYLAND(d) => d.is_primary(),
            #[cfg(feature = "drm")]
            Display::DRM(d) => d.is_primary(),
        }
    }

    pub fn name(&self) -> String {
        match self {
            Display::X11(d) => d.name(),
            Display::WAYLAND(d) => d.name(),
            #[cfg(feature = "drm")]
            Display::DRM(d) => d.name(),
        }
    }
}
