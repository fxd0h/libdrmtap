//! DRM/KMS screen capture backend for Linux, powered by libdrmtap.
//!
//! This backend captures the screen directly from the GPU framebuffer via
//! DRM/KMS ioctls, bypassing X11, Wayland, and PipeWire entirely. It works
//! on headless systems, VMs, and any Linux with a DRM-capable GPU.
//!
//! Requires:
//!   - libdrmtap.so installed (see https://github.com/fxd0h/libdrmtap)
//!   - CAP_SYS_ADMIN capability or root
//!   - /dev/dri/card* accessible

use hbb_common::log;
use crate::common::TraitCapturer;
use std::{io, time::Duration, time::Instant, process::Command};

use super::x11::PixelBuffer;

/// Whether the hardware cursor is composited into the captured frame.
/// DRM primary plane does not include the cursor plane.
pub const IS_CURSOR_EMBEDDED: bool = false;

// ---------------------------------------------------------------------------
// FFI bindings to libdrmtap
// ---------------------------------------------------------------------------

use libdrmtap_sys as ffi;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Build a `drmtap_config` from environment variables.
/// - `DRM_DEVICE`: explicit /dev/dri/cardN path (optional, auto-detects).
/// - `DRMTAP_DEBUG`: set to "1" to enable libdrmtap debug logging.
fn build_config() -> (Option<std::ffi::CString>, ffi::drmtap_config) {
    let device_env = std::env::var("DRM_DEVICE").ok();
    let device_cstr = device_env.map(|s| std::ffi::CString::new(s).unwrap());
    let cfg = ffi::drmtap_config {
        device_path: device_cstr
            .as_ref()
            .map(|c| c.as_ptr())
            .unwrap_or(std::ptr::null()),
        crtc_id: 0,
        helper_path: std::ptr::null(),
        debug: if std::env::var("DRMTAP_DEBUG").is_ok() {
            1
        } else {
            0
        },
    };
    (device_cstr, cfg)
}

/// Open a temporary drmtap context (for enumeration).
fn open_context() -> io::Result<*mut ffi::drmtap_ctx> {
    // Ignore SIGPIPE to prevent death from helper socket communication.
    // The helper uses a UNIX socketpair; if it closes its end while the
    // parent is reading, the default SIGPIPE action kills the process.
    {
        use std::sync::Once;
        static SIGPIPE_ONCE: Once = Once::new();
        SIGPIPE_ONCE.call_once(|| {
            unsafe {
                #[cfg(unix)]
                {
                    extern "C" { fn signal(sig: i32, handler: usize) -> usize; }
                    signal(13, 1); // 13 = SIGPIPE, 1 = SIG_IGN
                    eprintln!("[DIAG] SIGPIPE set to SIG_IGN");
                }
            }
        });
    }
    let (_cstr, cfg) = build_config();
    let ctx = unsafe { ffi::drmtap_open(&cfg) };
    if ctx.is_null() {
        Err(io::Error::new(
            io::ErrorKind::NotFound,
            "drmtap_open failed — check DRM_DEVICE and CAP_SYS_ADMIN",
        ))
    } else {
        Ok(ctx)
    }
}

/// Copy frame data from the mapped buffer into a tightly-packed BGRA buffer.
fn copy_frame_data(
    src: *const u8,
    w: usize,
    h: usize,
    stride: usize,
    dst: &mut Vec<u8>,
) {
    let frame_size = w * 4 * h;
    if dst.len() != frame_size {
        dst.resize(frame_size, 0);
    }
    unsafe {
        if stride == w * 4 {
            std::ptr::copy_nonoverlapping(src, dst.as_mut_ptr(), frame_size);
        } else {
            for y in 0..h {
                std::ptr::copy_nonoverlapping(
                    src.add(y * stride),
                    dst.as_mut_ptr().add(y * w * 4),
                    w * 4,
                );
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

pub struct Display {
    name: String,
    w: usize,
    h: usize,
    is_primary: bool,
}

impl Display {
    pub fn all() -> io::Result<Vec<Display>> {
        let ctx = open_context()?;
        let mut raw: [ffi::drmtap_display; 8] = unsafe { std::mem::zeroed() };
        let n = unsafe { ffi::drmtap_list_displays(ctx, raw.as_mut_ptr(), 8) };
        unsafe { ffi::drmtap_close(ctx) };

        if n <= 0 {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                "No active DRM displays found",
            ));
        }

        let displays: Vec<Display> = (0..n as usize)
            .enumerate()
            .map(|(idx, i)| {
                let name_bytes: Vec<u8> = raw[i]
                    .name
                    .iter()
                    .take_while(|&&c| c != 0)
                    .map(|&c| c as u8)
                    .collect();
                Display {
                    name: String::from_utf8_lossy(&name_bytes).to_string(),
                    w: if raw[i].width == 0 { 1920 } else { raw[i].width as usize },
                    h: if raw[i].height == 0 { 1080 } else { raw[i].height as usize },
                    is_primary: idx == 0,
                }
            })
            .collect();

        if displays.is_empty() {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                "No active DRM displays found",
            ));
        }

        log::info!("DRM: found {} display(s)", displays.len());
        Ok(displays)
    }

    pub fn primary() -> io::Result<Display> {
        let mut all = Self::all()?;
        Ok(all.remove(0))
    }

    pub fn width(&self) -> usize {
        self.w
    }

    pub fn height(&self) -> usize {
        self.h
    }

    pub fn scale(&self) -> f64 {
        1.0
    }

    pub fn logical_width(&self) -> usize {
        self.w
    }

    pub fn logical_height(&self) -> usize {
        self.h
    }

    pub fn origin(&self) -> (i32, i32) {
        (0, 0)
    }

    pub fn is_online(&self) -> bool {
        true
    }

    pub fn is_primary(&self) -> bool {
        self.is_primary
    }

    pub fn name(&self) -> String {
        self.name.clone()
    }
}

// ---------------------------------------------------------------------------
// Capturer
// ---------------------------------------------------------------------------

pub struct Capturer {
    ctx: *mut ffi::drmtap_ctx,
    w: usize,
    h: usize,
    buffer: Vec<u8>,
    consecutive_errors: u32,
    last_wake_attempt: Option<Instant>,
}

/// Maximum consecutive capture errors before bailing out.
const MAX_CONSECUTIVE_ERRORS: u32 = 50;
/// Maximum retry attempts when opening a new capture context.
const MAX_OPEN_RETRIES: u32 = 3;

impl Capturer {
    pub fn new(display: Display) -> io::Result<Capturer> {
        eprintln!("[DIAG] drm::Capturer::new() ENTERED for {}x{} ({})", display.w, display.h, display.name);
        log::info!(
            "DRM: opening capturer for {}x{} ({})",
            display.w,
            display.h,
            display.name
        );

        for attempt in 0..MAX_OPEN_RETRIES {
            if attempt > 0 {
                log::debug!("DRM: retry attempt {}", attempt + 1);
                std::thread::sleep(Duration::from_millis(200 * (attempt as u64 + 1)));
            }

            let ctx = match open_context() {
                Ok(c) => { eprintln!("[DIAG] drm: open_context() OK ctx={:?}", c); c },
                Err(e) => { eprintln!("[DIAG] drm: open_context() FAILED: {}", e); continue; },
            };

            // Validate with a test grab
            let mut test: ffi::drmtap_frame_info = unsafe { std::mem::zeroed() };
            eprintln!("[DIAG] drm: about to call drmtap_grab_mapped(ctx={:?})...", ctx);
            let ret = unsafe { ffi::drmtap_grab_mapped(ctx, &mut test) };
            eprintln!("[DIAG] drm: drmtap_grab_mapped RETURNED ret={} data_null={} w={} h={} stride={}",
                ret, test.data.is_null(), test.width, test.height, test.stride);
            if ret < 0 && ret != -19 {
                log::warn!("DRM: validation grab failed (ret={})", ret);
                unsafe { ffi::drmtap_close(ctx) };
                continue;
            }

            let mut capturer = Capturer {
                ctx,
                w: display.w,
                h: display.h,
                buffer: Vec::new(),
                consecutive_errors: 0,
                last_wake_attempt: None,
            };

            // Pre-fill buffer with first captured frame
            if ret == 0 && !test.data.is_null() && test.width > 0 {
                let w = test.width as usize;
                let h = test.height as usize;
                let stride = test.stride as usize;
                eprintln!("[DIAG] drm: copy_frame_data START {}x{} stride={} bytes={}", w, h, stride, w*4*h);
                copy_frame_data(test.data as *const u8, w, h, stride, &mut capturer.buffer);
                eprintln!("[DIAG] drm: copy_frame_data DONE buf_len={}", capturer.buffer.len());
                capturer.w = w;
                capturer.h = h;
            }

            eprintln!("[DIAG] drm::Capturer::new() SUCCESS attempt={} {}x{}", attempt+1, capturer.w, capturer.h);
            log::info!(
                "DRM: capturer ready (attempt {}, {}x{})",
                attempt + 1,
                capturer.w,
                capturer.h
            );
            return Ok(capturer);
        }

        Err(io::Error::new(
            io::ErrorKind::Other,
            format!(
                "DRM: capture context failed after {} attempts",
                MAX_OPEN_RETRIES
            ),
        ))
    }

    pub fn width(&self) -> usize {
        self.w
    }

    pub fn height(&self) -> usize {
        self.h
    }
}

/// Try to wake up the display by unlocking the GNOME session.
/// GNOME disables the CRTC when idle, making DRM capture impossible.
fn try_wake_display() {
    log::info!("DRM: attempting to wake display via loginctl + D-Bus...");
    let _ = Command::new("loginctl")
        .arg("unlock-sessions")
        .output();
    let _ = Command::new("dbus-send")
        .args(&[
            "--session",
            "--dest=org.gnome.ScreenSaver",
            "--type=method_call",
            "/org/gnome/ScreenSaver",
            "org.gnome.ScreenSaver.SetActive",
            "boolean:false",
        ])
        .env("DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/1000/bus")
        .output();
    if let Ok(output) = Command::new("loginctl")
        .args(&["list-sessions", "--no-legend"])
        .output()
    {
        if let Ok(s) = String::from_utf8(output.stdout) {
            if let Some(sid) = s.split_whitespace().next() {
                let _ = Command::new("loginctl")
                    .args(&["activate", sid])
                    .output();
            }
        }
    }
}

impl TraitCapturer for Capturer {
    fn frame<'a>(&'a mut self, _timeout: Duration) -> io::Result<crate::Frame<'a>> {
        let mut frame: ffi::drmtap_frame_info = unsafe { std::mem::zeroed() };
        let ret = unsafe { ffi::drmtap_grab_mapped(self.ctx, &mut frame) };

        static FRAME_COUNTER: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let fnum = FRAME_COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        if fnum < 5 || fnum % 60 == 0 {
            eprintln!("[DIAG] frame() #{}: ret={} data_null={} w={} h={} stride={} dma_buf_fd={} modifier=0x{:x}",
                fnum, ret, frame.data.is_null(), frame.width, frame.height,
                frame.stride, frame.dma_buf_fd, frame.modifier);
        }

        // -ENODEV means display is asleep / no active CRTC.
        // Try to wake the display periodically (every 5 seconds max).
        if ret == -19 {
            let should_wake = match self.last_wake_attempt {
                None => true,
                Some(t) => t.elapsed() > Duration::from_secs(5),
            };
            if should_wake {
                try_wake_display();
                self.last_wake_attempt = Some(Instant::now());
            }
            std::thread::sleep(Duration::from_millis(100));
            return Err(io::ErrorKind::WouldBlock.into());
        }

        if ret < 0 {
            self.consecutive_errors += 1;
            if self.consecutive_errors <= 3 {
                log::warn!("DRM: grab error (ret={}, count={})", ret, self.consecutive_errors);
            }
            if self.consecutive_errors >= MAX_CONSECUTIVE_ERRORS {
                log::error!("DRM: {} consecutive errors, bailing out", MAX_CONSECUTIVE_ERRORS);
                return Err(io::Error::new(io::ErrorKind::Other, "DRM capture failed"));
            }
            return Err(io::ErrorKind::WouldBlock.into());
        }
        self.consecutive_errors = 0;

        if frame.data.is_null() || frame.width == 0 || frame.height == 0 {
            log::warn!("DRM: got null/empty frame data (w={}, h={}, data_null={})", frame.width, frame.height, frame.data.is_null());
            std::thread::sleep(Duration::from_millis(100));
            return Err(io::ErrorKind::WouldBlock.into());
        }

        let w = frame.width as usize;
        let h = frame.height as usize;
        let stride = frame.stride as usize;
        // Frame captured successfully
        let needed = w * h * 4; if self.buffer.len() < needed { self.buffer.resize(needed, 0); } copy_frame_data(frame.data as *const u8, w, h, stride, &mut self.buffer);
        self.w = w;
        self.h = h;

        Ok(crate::Frame::PixelBuffer(PixelBuffer::new(
            &self.buffer,
            crate::Pixfmt::BGRA,
            w,
            h,
        )))
    }
}

impl Drop for Capturer {
    fn drop(&mut self) {
        log::debug!("DRM: closing capturer");
        if !self.ctx.is_null() {
            unsafe { ffi::drmtap_close(self.ctx) };
            self.ctx = std::ptr::null_mut();
        }
    }
}
