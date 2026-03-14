/*
 * libdrmtap — Safe Rust wrapper for DRM/KMS screen capture
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

//! # libdrmtap
//!
//! Safe Rust wrapper around the libdrmtap C library for DRM/KMS screen capture.
//!
//! ## Example
//!
//! ```rust,no_run
//! use libdrmtap::DrmTap;
//!
//! let mut tap = DrmTap::open(None).expect("Failed to open DRM device");
//!
//! // List displays
//! let displays = tap.list_displays().unwrap();
//! for d in &displays {
//!     println!("{}: {}x{}", d.name, d.width, d.height);
//! }
//!
//! // Capture a frame
//! let frame = tap.grab_mapped().expect("Capture failed");
//! println!("{}x{} stride={}", frame.width, frame.height, frame.stride);
//!
//! // Access pixel data as a byte slice
//! if let Some(pixels) = frame.data() {
//!     println!("First pixel: {:02x}{:02x}{:02x}",
//!              pixels[2], pixels[1], pixels[0]); // RGB from XRGB
//! }
//! ```

use std::ffi::{CStr, CString};
use std::os::raw::c_int;
use std::ptr;

use libdrmtap_sys as ffi;

/// Error type for libdrmtap operations
#[derive(Debug)]
pub struct Error {
    pub code: i32,
    pub message: String,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "drmtap error {}: {}", self.code, self.message)
    }
}

impl std::error::Error for Error {}

type Result<T> = std::result::Result<T, Error>;

fn check(ctx: *const ffi::drmtap_ctx, ret: c_int) -> Result<()> {
    if ret >= 0 {
        Ok(())
    } else {
        let msg = unsafe {
            let p = ffi::drmtap_error(ctx);
            if p.is_null() {
                "unknown error".to_string()
            } else {
                CStr::from_ptr(p).to_string_lossy().into_owned()
            }
        };
        Err(Error {
            code: ret as i32,
            message: msg,
        })
    }
}

/// Information about a connected display
#[derive(Debug, Clone)]
pub struct Display {
    pub crtc_id: u32,
    pub connector_id: u32,
    pub name: String,
    pub width: u32,
    pub height: u32,
    pub refresh_hz: u32,
    pub active: bool,
}

/// Configuration for opening a capture context
pub struct Config {
    pub device_path: Option<String>,
    pub crtc_id: u32,
    pub helper_path: Option<String>,
    pub debug: bool,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            device_path: None,
            crtc_id: 0,
            helper_path: None,
            debug: false,
        }
    }
}

/// DRM/KMS screen capture context
///
/// This is the main entry point for the library. Create one with
/// `DrmTap::open()`, then use `grab()` or `grab_mapped()` to capture frames.
pub struct DrmTap {
    ctx: *mut ffi::drmtap_ctx,
}

// SAFETY: The C library uses pthread_mutex for thread safety
unsafe impl Send for DrmTap {}

impl DrmTap {
    /// Open a DRM device for capture.
    ///
    /// Pass `None` for auto-detection, or `Some(config)` for specific settings.
    pub fn open(config: Option<Config>) -> Result<Self> {
        let ctx = match config {
            None => unsafe { ffi::drmtap_open(ptr::null()) },
            Some(cfg) => {
                let device = cfg.device_path.as_ref().map(|s| CString::new(s.as_str()).unwrap());
                let helper = cfg.helper_path.as_ref().map(|s| CString::new(s.as_str()).unwrap());

                let raw_cfg = ffi::drmtap_config {
                    device_path: device.as_ref().map_or(ptr::null(), |c| c.as_ptr()),
                    crtc_id: cfg.crtc_id,
                    helper_path: helper.as_ref().map_or(ptr::null(), |c| c.as_ptr()),
                    debug: if cfg.debug { 1 } else { 0 },
                };
                unsafe { ffi::drmtap_open(&raw_cfg) }
            }
        };

        if ctx.is_null() {
            let msg = unsafe {
                let p = ffi::drmtap_error(ptr::null());
                if p.is_null() {
                    "failed to open DRM device".to_string()
                } else {
                    CStr::from_ptr(p).to_string_lossy().into_owned()
                }
            };
            Err(Error {
                code: -1,
                message: msg,
            })
        } else {
            Ok(DrmTap { ctx })
        }
    }

    /// List connected displays.
    pub fn list_displays(&mut self) -> Result<Vec<Display>> {
        let mut raw = vec![unsafe { std::mem::zeroed::<ffi::drmtap_display>() }; 16];
        let n = unsafe { ffi::drmtap_list_displays(self.ctx, raw.as_mut_ptr(), 16) };
        check(self.ctx, n)?;

        let count = n as usize;
        Ok(raw[..count]
            .iter()
            .map(|d| {
                let name = unsafe {
                    CStr::from_ptr(d.name.as_ptr())
                        .to_string_lossy()
                        .into_owned()
                };
                Display {
                    crtc_id: d.crtc_id,
                    connector_id: d.connector_id,
                    name,
                    width: d.width,
                    height: d.height,
                    refresh_hz: d.refresh_hz,
                    active: d.active != 0,
                }
            })
            .collect())
    }

    /// Check if display configuration changed (hotplug).
    pub fn hotplug_changed(&mut self) -> bool {
        unsafe { ffi::drmtap_hotplug_changed(self.ctx) != 0 }
    }

    /// Capture a frame (zero-copy — DMA-BUF fd only).
    pub fn grab(&mut self) -> Result<Frame> {
        let mut raw = unsafe { std::mem::zeroed::<ffi::drmtap_frame_info>() };
        let ret = unsafe { ffi::drmtap_grab(self.ctx, &mut raw) };
        check(self.ctx, ret)?;
        Ok(Frame {
            ctx: self.ctx,
            raw,
        })
    }

    /// Capture a frame with mapped pixel data.
    pub fn grab_mapped(&mut self) -> Result<Frame> {
        let mut raw = unsafe { std::mem::zeroed::<ffi::drmtap_frame_info>() };
        let ret = unsafe { ffi::drmtap_grab_mapped(self.ctx, &mut raw) };
        check(self.ctx, ret)?;
        Ok(Frame {
            ctx: self.ctx,
            raw,
        })
    }

    /// Get the GPU driver name (e.g., "i915", "amdgpu", "virtio_gpu").
    pub fn gpu_driver(&mut self) -> Option<String> {
        let p = unsafe { ffi::drmtap_gpu_driver(self.ctx) };
        if p.is_null() {
            None
        } else {
            Some(unsafe { CStr::from_ptr(p).to_string_lossy().into_owned() })
        }
    }

    /// Get the last error message.
    pub fn error(&self) -> Option<String> {
        let p = unsafe { ffi::drmtap_error(self.ctx) };
        if p.is_null() {
            None
        } else {
            let s = unsafe { CStr::from_ptr(p).to_string_lossy().into_owned() };
            if s.is_empty() {
                None
            } else {
                Some(s)
            }
        }
    }

    /// Get the cursor state (position, image, visibility).
    pub fn get_cursor(&mut self) -> Result<Cursor> {
        let mut raw = unsafe { std::mem::zeroed::<ffi::drmtap_cursor_info>() };
        let ret = unsafe { ffi::drmtap_get_cursor(self.ctx, &mut raw) };
        check(self.ctx, ret)?;
        Ok(Cursor {
            ctx: self.ctx,
            raw,
        })
    }
}

impl Drop for DrmTap {
    fn drop(&mut self) {
        unsafe { ffi::drmtap_close(self.ctx) };
    }
}

/// A captured frame. Automatically released on drop.
pub struct Frame {
    ctx: *mut ffi::drmtap_ctx,
    raw: ffi::drmtap_frame_info,
}

impl Frame {
    /// Frame width in pixels
    pub fn width(&self) -> u32 {
        self.raw.width
    }

    /// Frame height in pixels
    pub fn height(&self) -> u32 {
        self.raw.height
    }

    /// Stride (bytes per row)
    pub fn stride(&self) -> u32 {
        self.raw.stride
    }

    /// DRM fourcc pixel format
    pub fn format(&self) -> u32 {
        self.raw.format
    }

    /// DRM format modifier
    pub fn modifier(&self) -> u64 {
        self.raw.modifier
    }

    /// DMA-BUF file descriptor (for zero-copy)
    pub fn dma_buf_fd(&self) -> i32 {
        self.raw.dma_buf_fd as i32
    }

    /// Access mapped pixel data as a byte slice.
    ///
    /// Returns `None` if the frame was captured with `grab()` (zero-copy)
    /// or if mmap failed.
    pub fn data(&self) -> Option<&[u8]> {
        if self.raw.data.is_null() {
            None
        } else {
            let len = self.raw.stride as usize * self.raw.height as usize;
            Some(unsafe { std::slice::from_raw_parts(self.raw.data as *const u8, len) })
        }
    }
}

impl Drop for Frame {
    fn drop(&mut self) {
        unsafe { ffi::drmtap_frame_release(self.ctx, &mut self.raw) };
    }
}

/// Cursor state. Automatically released on drop.
pub struct Cursor {
    ctx: *mut ffi::drmtap_ctx,
    raw: ffi::drmtap_cursor_info,
}

impl Cursor {
    /// Cursor x position on screen
    pub fn x(&self) -> i32 {
        self.raw.x
    }

    /// Cursor y position on screen
    pub fn y(&self) -> i32 {
        self.raw.y
    }

    /// Hotspot x offset within cursor image
    pub fn hot_x(&self) -> i32 {
        self.raw.hot_x
    }

    /// Hotspot y offset within cursor image
    pub fn hot_y(&self) -> i32 {
        self.raw.hot_y
    }

    /// Cursor image width
    pub fn width(&self) -> u32 {
        self.raw.width
    }

    /// Cursor image height
    pub fn height(&self) -> u32 {
        self.raw.height
    }

    /// Whether cursor is visible
    pub fn visible(&self) -> bool {
        self.raw.visible != 0
    }

    /// Cursor pixel data as ARGB8888 u32 slice.
    ///
    /// Returns `None` if cursor is hidden.
    pub fn pixels(&self) -> Option<&[u32]> {
        if self.raw.pixels.is_null() {
            None
        } else {
            let len = self.raw.width as usize * self.raw.height as usize;
            Some(unsafe { std::slice::from_raw_parts(self.raw.pixels, len) })
        }
    }
}

impl Drop for Cursor {
    fn drop(&mut self) {
        unsafe { ffi::drmtap_cursor_release(self.ctx, &mut self.raw) };
    }
}

/// Get the library version as a packed integer: `(major << 16) | (minor << 8) | patch`
pub fn version() -> (u8, u8, u8) {
    let v = unsafe { ffi::drmtap_version() } as u32;
    ((v >> 16) as u8, ((v >> 8) & 0xFF) as u8, (v & 0xFF) as u8)
}
