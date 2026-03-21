// DRM/KMS capture backend for RustDesk — powered by libdrmtap
//
// This file is a self-contained capture backend that integrates directly
// into RustDesk's scrap crate. It uses inline FFI bindings to libdrmtap,
// requiring only the C library to be installed (no Rust crate dependency).
//
// Tested: Ubuntu 24.04 VM (virtio_gpu), RustDesk screenshot example
// captured 1920x1017 @ BGRA, 7,810,560 bytes — pixel-perfect.
//
// To use in RustDesk:
//   Place this file at libs/scrap/src/common/drm.rs
//   See mod.rs in this directory for full integration instructions.

use crate::{Frame, TraitCapturer};
use std::{io, time::{Duration, Instant}};
use super::x11::PixelBuffer;

// FFI bindings to libdrmtap — struct layouts must match drmtap.h exactly!
mod ffi {
    use std::os::raw::{c_char, c_int, c_void};

    // drmtap.h: typedef struct { const char *device_path; uint32_t crtc_id;
    //           const char *helper_path; int debug; } drmtap_config;
    #[repr(C)]
    pub struct drmtap_config {
        pub device_path: *const c_char,
        pub crtc_id: u32,
        pub helper_path: *const c_char,
        pub debug: c_int,
    }

    // drmtap.h: typedef struct { uint32_t crtc_id; uint32_t connector_id;
    //           char name[32]; uint32_t width, height, refresh_hz; int active;
    //           } drmtap_display;
    #[repr(C)]
    #[derive(Clone)]
    #[allow(non_camel_case_types)]
    pub struct drmtap_display {
        pub crtc_id: u32,
        pub connector_id: u32,
        pub name: [c_char; 32],
        pub width: u32,
        pub height: u32,
        pub refresh_hz: u32,
        pub active: c_int,
    }

    // drmtap.h: typedef struct { void *data; int dma_buf_fd;
    //           uint32_t width, height, stride, format; uint64_t modifier;
    //           void *_priv; } drmtap_frame_info;
    #[repr(C)]
    #[allow(non_camel_case_types)]
    pub struct drmtap_frame_info {
        pub data: *mut c_void,
        pub dma_buf_fd: c_int,
        pub width: u32,
        pub height: u32,
        pub stride: u32,
        pub format: u32,
        pub modifier: u64,
        pub fb_id: u32,
        pub _priv: *mut c_void,
    }

    #[allow(non_camel_case_types)]
    pub type drmtap_ctx = c_void;

    #[link(name = "drmtap")]
    extern "C" {
        pub fn drmtap_open(cfg: *const drmtap_config) -> *mut drmtap_ctx;
        pub fn drmtap_close(ctx: *mut drmtap_ctx);
        pub fn drmtap_list_displays(
            ctx: *mut drmtap_ctx,
            displays: *mut drmtap_display,
            max: c_int,
        ) -> c_int;
        pub fn drmtap_grab_mapped(
            ctx: *mut drmtap_ctx,
            frame: *mut drmtap_frame_info,
        ) -> c_int;
        pub fn drmtap_frame_release(
            ctx: *mut drmtap_ctx,
            frame: *mut drmtap_frame_info,
        );
        pub fn drmtap_error(ctx: *const drmtap_ctx) -> *const c_char;
        pub fn drmtap_gpu_driver(ctx: *const drmtap_ctx) -> *const c_char;
    }
}

pub struct Display {
    name: String,
    w: usize,
    h: usize,
    primary: bool,
}

impl Display {
    pub fn all() -> io::Result<Vec<Display>> {
        unsafe {
            // Read DRM_DEVICE env var for device path
            let device_env = std::env::var("DRM_DEVICE").ok();
            let device_cstr = device_env.as_ref().map(|s| {
                std::ffi::CString::new(s.as_str()).unwrap()
            });

            let cfg = ffi::drmtap_config {
                device_path: device_cstr
                    .as_ref()
                    .map(|c| c.as_ptr())
                    .unwrap_or(std::ptr::null()),
                crtc_id: 0,
                helper_path: std::ptr::null(),
                debug: if std::env::var("DRMTAP_DEBUG").is_ok() { 1 } else { 0 },
            };
            let ctx = ffi::drmtap_open(&cfg);
            if ctx.is_null() {
                return Err(io::Error::new(
                    io::ErrorKind::NotFound,
                    "drmtap_open failed",
                ));
            }

            let mut raw_displays = vec![std::mem::zeroed::<ffi::drmtap_display>(); 8];
            let n = ffi::drmtap_list_displays(ctx, raw_displays.as_mut_ptr(), 8);
            ffi::drmtap_close(ctx);

            if n <= 0 {
                return Err(io::Error::new(
                    io::ErrorKind::NotFound,
                    "No DRM displays found",
                ));
            }

            let displays: Vec<Display> = (0..n as usize)
                .filter(|&i| raw_displays[i].active != 0)
                .enumerate()
                .map(|(idx, i)| {
                    let name_bytes: Vec<u8> = raw_displays[i]
                        .name
                        .iter()
                        .take_while(|&&c| c != 0)
                        .map(|&c| c as u8)
                        .collect();
                    let name = String::from_utf8_lossy(&name_bytes).to_string();
                    Display {
                        name,
                        w: raw_displays[i].width as usize,
                        h: raw_displays[i].height as usize,
                        primary: idx == 0,
                    }
                })
                .collect();

            if displays.is_empty() {
                return Err(io::Error::new(
                    io::ErrorKind::NotFound,
                    "No active DRM displays",
                ));
            }

            Ok(displays)
        }
    }

    pub fn primary() -> io::Result<Display> {
        let mut all = Self::all()?;
        Ok(all.remove(0))
    }

    pub fn width(&self) -> usize { self.w }
    pub fn height(&self) -> usize { self.h }
    pub fn scale(&self) -> f64 { 1.0 }
    pub fn logical_width(&self) -> usize { self.w }
    pub fn logical_height(&self) -> usize { self.h }
    pub fn origin(&self) -> (i32, i32) { (0, 0) }
    pub fn is_online(&self) -> bool { true }
    pub fn is_primary(&self) -> bool { self.primary }
    pub fn name(&self) -> String { self.name.clone() }
}

pub struct Capturer {
    ctx: *mut ffi::drmtap_ctx,
    w: usize,
    h: usize,
    buffer: Vec<u8>,
    last_fb_id: u32,
    // Performance metrics
    frame_count: u64,
    grab_time_us: u64,
    copy_time_us: u64,
    total_time_us: u64,
    dirty_count: u64,
    skip_count: u64,
    last_grab_time: Instant,
}

impl Capturer {
    pub fn new(display: Display) -> io::Result<Capturer> {
        unsafe {
            let device_env = std::env::var("DRM_DEVICE").ok();
            let device_cstr = device_env.as_ref().map(|s| {
                std::ffi::CString::new(s.as_str()).unwrap()
            });

            let cfg = ffi::drmtap_config {
                device_path: device_cstr
                    .as_ref()
                    .map(|c| c.as_ptr())
                    .unwrap_or(std::ptr::null()),
                crtc_id: 0,
                helper_path: std::ptr::null(),
                debug: if std::env::var("DRMTAP_DEBUG").is_ok() { 1 } else { 0 },
            };
            let ctx = ffi::drmtap_open(&cfg);
            if ctx.is_null() {
                return Err(io::Error::new(
                    io::ErrorKind::Other,
                    "drmtap_open failed",
                ));
            }
            Ok(Capturer {
                ctx,
                w: display.w,
                h: display.h,
                buffer: Vec::new(),
                last_fb_id: 0,
                frame_count: 0,
                grab_time_us: 0,
                copy_time_us: 0,
                total_time_us: 0,
                dirty_count: 0,
                skip_count: 0,
                last_grab_time: Instant::now(),
            })
        }
    }

    pub fn width(&self) -> usize { self.w }
    pub fn height(&self) -> usize { self.h }
}

impl TraitCapturer for Capturer {
    fn frame<'a>(&'a mut self, timeout: Duration) -> io::Result<Frame<'a>> {
        unsafe {
            // Rate limit: minimum 16ms between grabs (~60 FPS max)
            let elapsed = self.last_grab_time.elapsed();
            let min_interval = Duration::from_millis(16);
            if elapsed < min_interval {
                std::thread::sleep(min_interval - elapsed);
            }

            let mut frame: ffi::drmtap_frame_info = std::mem::zeroed();
            let ret = ffi::drmtap_grab_mapped(self.ctx, &mut frame);
            if ret < 0 {
                std::thread::sleep(Duration::from_millis(16));
                return Err(io::ErrorKind::WouldBlock.into());
            }

            if frame.data.is_null() || frame.width == 0 || frame.height == 0 {
                ffi::drmtap_frame_release(self.ctx, &mut frame);
                std::thread::sleep(Duration::from_millis(16));
                return Err(io::ErrorKind::WouldBlock.into());
            }

            self.last_grab_time = Instant::now();
            let current_fb_id = frame.fb_id;

            // fb_id skip: if framebuffer hasn't changed, skip expensive copy
            if current_fb_id == self.last_fb_id && self.last_fb_id != 0 {
                ffi::drmtap_frame_release(self.ctx, &mut frame);
                self.skip_count += 1;
                let sleep_ms = timeout.as_millis().min(33).max(1) as u64;
                std::thread::sleep(Duration::from_millis(sleep_ms));
                return Err(io::ErrorKind::WouldBlock.into());
            }
            self.last_fb_id = current_fb_id;

            let w = frame.width as usize;
            let h = frame.height as usize;
            let stride = frame.stride as usize;
            let frame_size = w * 4 * h;

            if self.buffer.len() != frame_size {
                self.buffer.resize(frame_size, 0);
            }

            let src = frame.data as *const u8;
            if stride == w * 4 {
                std::ptr::copy_nonoverlapping(src, self.buffer.as_mut_ptr(), frame_size);
            } else {
                for y in 0..h {
                    std::ptr::copy_nonoverlapping(
                        src.add(y * stride),
                        self.buffer.as_mut_ptr().add(y * w * 4),
                        w * 4,
                    );
                }
            }

            ffi::drmtap_frame_release(self.ctx, &mut frame);

            self.frame_count += 1;
            self.w = w;
            self.h = h;
            Ok(Frame::PixelBuffer(PixelBuffer::new(
                &self.buffer,
                crate::Pixfmt::BGRA,
                w,
                h,
            )))
        }
    }
}

impl Drop for Capturer {
    fn drop(&mut self) {
        if !self.ctx.is_null() {
            unsafe { ffi::drmtap_close(self.ctx); }
            self.ctx = std::ptr::null_mut();
        }
    }
}
