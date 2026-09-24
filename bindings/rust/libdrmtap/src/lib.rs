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
//! println!("{}x{} stride={}", frame.width(), frame.height(), frame.stride());
//!
//! // Access pixel data as a byte slice
//! if let Some(pixels) = frame.data() {
//!     println!("First pixel: {:02x}{:02x}{:02x}",
//!              pixels[2], pixels[1], pixels[0]); // RGB from XRGB
//! }
//! ```

use std::ffi::{CStr, CString};
use std::fmt;
use std::io;
use std::os::fd::{BorrowedFd, OwnedFd, RawFd};
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

impl Error {
    /// The error as an [`io::Error`], when it really is an errno.
    ///
    /// Most of the C API returns a negative errno (`-EINVAL`, `-ENOTSUP`, `-EIO`, `-ENODEV`,
    /// `-ENOMEM`, `-EFBIG`, `-EACCES`, `-ENOSPC`, `-EPROTO`), and those convert exactly.
    ///
    /// `-1` is the one value that cannot be trusted, and it is genuinely ambiguous rather than
    /// merely a sentinel. `drmtap_drm_fd()` returns a bare `-1`, and so does this wrapper when
    /// `drmtap_open` hands back a null context. But `-1` is ALSO `-EPERM`, and the capture path
    /// returns `-errno` straight from the kernel (`drmModeGetFB2`, `drmPrimeHandleToFD`, the
    /// virtio transfer ioctls), so an unprivileged caller really can produce one. Nothing in the
    /// integer distinguishes the two, so this refuses to guess and answers `None`. Very little is
    /// lost by that: [`Error::message`] carries the C library's own text, which names the call and
    /// the strerror string.
    ///
    /// [`Error::code`] stays as it is: it is a public field, so changing its type is a breaking
    /// release. This accessor is the additive half.
    pub fn io_error(&self) -> Option<io::Error> {
        if self.code < -1 {
            Some(io::Error::from_raw_os_error(-self.code))
        } else {
            None
        }
    }
}

/// A DRM fourcc rendered the way it is written down, e.g. `XR24`.
///
/// Falls back to the hex value for a code that is not four printable ASCII bytes, so this never
/// produces something that looks like a format but is not one.
fn fourcc_str(code: u32) -> String {
    let bytes = code.to_le_bytes();
    if bytes
        .iter()
        .all(|b| b.is_ascii_graphic() || *b == b' ')
    {
        bytes.iter().map(|b| *b as char).collect()
    } else {
        format!("{code:#010x}")
    }
}

/// Everything the unprivileged side needs to import a captured scanout, MINUS the file descriptor.
///
/// The fd is deliberately absent. In C this struct carries one, but the header is explicit that it
/// "is an integer valid only in THIS (exporter) process — it aliases @p frame's fd", and the frame
/// owns it: handing that number out inside a plain struct would let it outlive the frame, and the
/// failure is worse than a dangling number, because the kernel can hand the same integer to an
/// unrelated `open()` and a later convert would read some other file. Take the fd from the frame
/// instead ([`Frame::dma_buf_borrowed_fd`], [`Frame::try_clone_fd`]), which is also the documented
/// flow: the descriptor is serialized and the fd travels out of band over `SCM_RIGHTS`.
///
/// `num_planes`/`offsets`/`pitches` are what [`DrmTap::grab`] cannot give you at all, and they are
/// what a compressed (Intel CCS) or HDR scanout needs to be imported losslessly.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DmabufDesc {
    /// Frame width in pixels
    pub width: u32,
    /// Frame height in pixels
    pub height: u32,
    /// DRM fourcc of the scanout
    pub format: u32,
    /// DRM format modifier (tiling/compression)
    pub modifier: u64,
    /// KMS framebuffer id, the import-once cache key; 0 disables caching for this frame
    pub fb_id: u32,
    /// Used entries in `offsets`/`pitches` (1..4); 0 is treated as 1
    pub num_planes: u32,
    /// Per-plane byte offsets into the DMA-BUF
    pub offsets: [u32; 4],
    /// Per-plane strides in bytes; `pitches[0]` is the main surface stride
    pub pitches: [u32; 4],
    /// EOTF of the scanout; PQ is what triggers the HDR to SDR tone-map on conversion
    pub hdr_eotf: u32,
    /// Mastering/content peak luminance in cd/m2, 0 = unknown
    pub hdr_max_nits: u32,
}

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
    pub x: u32,
    pub y: u32,
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

// SAFETY: `DrmTap` owns its `drmtap_ctx` exclusively and the context has no
// thread affinity, so moving ownership to another thread is sound. It is
// deliberately NOT `Sync`: the context is not internally synchronized, so it
// must not be shared across threads (use one `DrmTap` per thread, or guard a
// shared one with your own lock).
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
                    x: d.x,
                    y: d.y,
                    width: d.width,
                    height: d.height,
                    refresh_hz: d.refresh_hz,
                    active: d.active != 0,
                }
            })
            .collect())
    }

    /// Check if display configuration changed (hotplug).
    pub fn displays_changed(&mut self) -> bool {
        unsafe { ffi::drmtap_displays_changed(self.ctx) != 0 }
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

    /// Capture a frame AND the full descriptor the unprivileged converter needs.
    ///
    /// The zero-copy counterpart of [`DrmTap::grab`] for a SPLIT consumer: the returned
    /// [`DmabufDesc`] carries the plane layout and HDR state that [`Frame`] does not have, and
    /// without them a compressed (Intel CCS) or HDR scanout cannot be imported at all -- you would
    /// hold the fd and no way to know where the planes sit inside it.
    ///
    /// The frame owns the DMA-BUF: send the descriptor over IPC, send the fd out of band with
    /// `SCM_RIGHTS` (see [`Frame::dma_buf_borrowed_fd`] and [`Frame::try_clone_fd`]), and keep the
    /// frame alive until it has gone. On a frame from THIS call the fd is always there --
    /// `drmtap_grab_desc` fails closed with `-ENOTSUP` rather than hand back a descriptor no
    /// receiver could convert -- so `dma_buf_borrowed_fd()` is never `None` here, unlike on a
    /// frame from [`DrmTap::grab`].
    ///
    /// The receiving half is not wrapped: `drmtap_open_render`/`drmtap_convert_dmabuf` exist in
    /// `libdrmtap-sys` and not here, so the consumer that imports this descriptor is expected to
    /// be using the C API (or the sys crate). This call is the exporter's side of that split.
    pub fn grab_desc(&mut self) -> Result<(Frame, DmabufDesc)> {
        let mut raw = unsafe { std::mem::zeroed::<ffi::drmtap_frame_info>() };
        let mut desc = unsafe { std::mem::zeroed::<ffi::drmtap_dmabuf_desc>() };
        let ret = unsafe { ffi::drmtap_grab_desc(self.ctx, &mut desc, &mut raw) };
        check(self.ctx, ret)?;
        let out = DmabufDesc {
            width: desc.width,
            height: desc.height,
            format: desc.format,
            modifier: desc.modifier,
            fb_id: desc.fb_id,
            num_planes: desc.num_planes,
            offsets: desc.offsets,
            pitches: desc.pitches,
            hdr_eotf: desc.hdr_eotf,
            hdr_max_nits: desc.hdr_max_nits,
        };
        Ok((
            Frame {
                ctx: self.ctx,
                raw,
            },
            out,
        ))
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
    /// The DRM `rotation` bitmask the primary plane scans out with, read now.
    ///
    /// `Some(mask)` carries exactly one of `0x1` (0), `0x2` (90), `0x4` (180) or
    /// `0x8` (270), possibly with `0x10`/`0x20` for a reflection. `None` means the
    /// plane has no `rotation` property, so the compositor can only have rotated
    /// in software: treat it as 0. A frame has to be turned back by the output
    /// transform MINUS this rotation; see `drmtap_plane_rotation()` in the header.
    ///
    /// Available since 0.5.8.
    pub fn plane_rotation(&mut self) -> Result<Option<u32>> {
        let mut rotation: u32 = 0;
        let rc = unsafe { ffi::drmtap_plane_rotation(self.raw, &mut rotation) };
        if rc == 0 {
            Ok(Some(rotation))
        } else if rc == -95 {
            Ok(None)
        } else {
            check(self.raw, rc).map(|_| None)
        }
    }

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

    /// DMA-BUF file descriptor, borrowed from this frame.
    ///
    /// `None` on the mapped paths, where there is no transferable DMA-BUF and the C field holds
    /// `-1`; handing out a borrowed `-1` would be a lie. The borrow is tied to `&self` because that
    /// IS the contract: the frame's `Drop` calls `drmtap_frame_release`, which closes the fd, so
    /// using it after the frame is gone must not compile. Do not close it -- see
    /// [`Frame::try_clone_fd`] for an fd that outlives the frame.
    pub fn dma_buf_borrowed_fd(&self) -> Option<BorrowedFd<'_>> {
        let fd = self.raw.dma_buf_fd as RawFd;
        if fd < 0 {
            None
        } else {
            // SAFETY: the fd is owned by this frame and stays open until its Drop releases it, so
            // it is valid for the lifetime of the borrow.
            Some(unsafe { BorrowedFd::borrow_raw(fd) })
        }
    }

    /// A duplicate of the DMA-BUF fd, owned by the caller.
    ///
    /// For the case where the fd has to outlive the frame. It DUPS rather than handing over the
    /// frame's own descriptor, because that one is closed by `drmtap_frame_release` and giving it
    /// away would be a double close.
    ///
    /// The caveat that the type cannot express: duplicating the descriptor keeps the BUFFER
    /// mapping alive, but not the frame's claim on the scanout. Once the frame is released the
    /// compositor is free to recycle that buffer, so what the fd refers to may be a later frame,
    /// or be written under you. It is the right tool for handing the buffer to another process
    /// promptly, not for holding a still.
    ///
    /// Returns [`io::ErrorKind::InvalidInput`] on a mapped frame, which has no DMA-BUF.
    pub fn try_clone_fd(&self) -> io::Result<OwnedFd> {
        let borrowed = self.dma_buf_borrowed_fd().ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                "this frame has no dma-buf: it came from a mapped capture path",
            )
        })?;
        // F_DUPFD_CLOEXEC, and no unsafe: stable since 1.63, so it is inside the declared floor.
        borrowed.try_clone_to_owned()
    }

    /// DMA-BUF file descriptor as a raw integer.
    ///
    /// Kept for compatibility and unchanged in shape, since this is a patch release. It tells you
    /// nothing about ownership or lifetime, and `-1` means "no DMA-BUF" rather than being an fd:
    /// prefer [`Frame::dma_buf_borrowed_fd`], or [`Frame::try_clone_fd`] when it must outlive the
    /// frame.
    #[deprecated(
        since = "0.5.7",
        note = "use dma_buf_borrowed_fd() for a borrow tied to the frame, or try_clone_fd() for an \
                owned duplicate; this raw form cannot express either"
    )]
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

/// The typed format accessors, behind the optional `drm-fourcc` feature.
///
/// They are optional on purpose: putting a third-party type in a public signature ties this
/// crate's semver to that crate's, permanently, and this wrapper sits under a capture library
/// other people pin. `format()` stays a `u32` for everyone, the `Debug` output already prints the
/// readable fourcc with no dependency at all, and whoever wants the enum opts in.
#[cfg(feature = "drm-fourcc")]
impl Frame {
    /// The scanout's format and modifier as `drm_fourcc` types.
    ///
    /// `None` when the fourcc is not one this version of `drm_fourcc` knows.
    pub fn drm_format(&self) -> Option<drm_fourcc::DrmFormat> {
        drm_format_of(self.raw.format, self.raw.modifier)
    }
}

#[cfg(feature = "drm-fourcc")]
impl DmabufDesc {
    /// The scanout's format and modifier as `drm_fourcc` types; see [`Frame::drm_format`].
    pub fn drm_format(&self) -> Option<drm_fourcc::DrmFormat> {
        drm_format_of(self.format, self.modifier)
    }
}

#[cfg(feature = "drm-fourcc")]
fn drm_format_of(format: u32, modifier: u64) -> Option<drm_fourcc::DrmFormat> {
    Some(drm_fourcc::DrmFormat {
        code: drm_fourcc::DrmFourcc::try_from(format).ok()?,
        modifier: drm_fourcc::DrmModifier::from(modifier),
    })
}

impl fmt::Debug for Frame {
    /// The format is printed as its fourcc (`XR24`), not as the decimal the C struct holds, which
    /// is the whole reason this exists rather than a derive.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Frame")
            .field("width", &self.raw.width)
            .field("height", &self.raw.height)
            .field("stride", &self.raw.stride)
            .field("format", &fourcc_str(self.raw.format))
            .field("modifier", &format_args!("{:#018x}", self.raw.modifier))
            .field("fb_id", &self.raw.fb_id)
            .field("dma_buf", &self.dma_buf_borrowed_fd().is_some())
            .field("mapped", &!self.raw.data.is_null())
            .finish()
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
    /// X of the cursor IMAGE's top-left corner, not the click point: the plane's
    /// `CRTC_X`, in the CRTC's physical scanout pixels and relative to that CRTC,
    /// not to a multi-monitor desktop origin and not in logical (scaled) units.
    pub fn x(&self) -> i32 {
        self.raw.x
    }

    /// Y of the cursor IMAGE's top-left corner; see [`Cursor::x`].
    pub fn y(&self) -> i32 {
        self.raw.y
    }

    /// Hotspot x within the cursor image, or `0` when the driver does not expose one.
    ///
    /// `HOTSPOT_X`/`HOTSPOT_Y` are plane properties only para-virtualized drivers
    /// (virtio-gpu, vmwgfx, qxl, vboxvideo) create, so on bare metal (i915, amdgpu,
    /// nvidia) this is always `0`. Which of the two a `0` is — a missing property or
    /// a driver that really puts the hotspot at the corner — is NOT visible here:
    /// ask [`Cursor::hotspot_from_driver`]. A caller that injects the pointer itself
    /// can recover an absent value:
    /// the plane sits at the pointer minus the hotspot, so once both are still,
    /// `hotspot = injected_position - plane_position`. Convert first: this position is
    /// CRTC-relative physical pixels, while an injected point is usually in the
    /// compositor's logical layout, so map it into scanout space (subtract that
    /// output's origin, scale by physical over logical) before subtracting. Otherwise
    /// estimate the hotspot from the image, which costs about half a glyph on a wide
    /// centre-hotspot shape.
    pub fn hot_x(&self) -> i32 {
        self.raw.hot_x
    }

    /// Hotspot y within the cursor image; see [`Cursor::hot_x`] for when it is `0`.
    pub fn hot_y(&self) -> i32 {
        self.raw.hot_y
    }

    /// Whether [`Cursor::hot_x`]/[`Cursor::hot_y`] were read from the driver.
    ///
    /// `Some(true)` means both `HOTSPOT_X` and `HOTSPOT_Y` were present, so the
    /// pair is the driver's answer even when it is `(0, 0)`; `Some(false)` means at
    /// least one was absent, so those zeros carry no information and a consumer
    /// that needs a hotspot has to estimate one. `None` means nothing recorded an
    /// answer for this sample — a cursor read through a privileged helper older
    /// than the one shipped with this release — and it must NOT be folded into
    /// `Some(false)`: "nobody said" is not "it was a guess".
    ///
    /// Available since 0.5.6; `None` is what an older `libdrmtap.so` produces
    /// through this same call.
    pub fn hotspot_from_driver(&self) -> Option<bool> {
        let mut valid: std::os::raw::c_int = 0;
        let rc = unsafe { ffi::drmtap_cursor_hotspot_valid(&self.raw, &mut valid) };
        if rc == 0 {
            Some(valid != 0)
        } else {
            None
        }
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_fourcc_prints_the_way_it_is_written_down() {
        // XR24 is XRGB8888, the format every scanout here comes back as.
        assert_eq!(fourcc_str(0x3432_5258), "XR24");
        // AR24 is ARGB8888. Built from the bytes rather than a hand-written hex literal,
        // because the first version of this line had the nibbles wrong and asserted "AR88".
        assert_eq!(fourcc_str(u32::from_le_bytes(*b"AR24")), "AR24");
        // Three letters and a pad byte: the kernel writes them space-padded, not NUL-padded.
        assert_eq!(fourcc_str(u32::from_le_bytes(*b"C8  ")), "C8  ");
        // Not four printable bytes: say the number rather than emit something that LOOKS like a
        // fourcc. A NUL byte is the giveaway that this is not one.
        assert_eq!(fourcc_str(0), "0x00000000");
        assert_eq!(fourcc_str(0x0000_3432), "0x00003432");
    }

    #[test]
    fn only_a_real_errno_becomes_an_io_error() {
        // What the public C API actually returns.
        let e = Error { code: -22, message: "bad argument".into() };
        assert_eq!(e.io_error().map(|io| io.raw_os_error()), Some(Some(22)));
        assert_eq!(
            Error { code: -19, message: String::new() }
                .io_error()
                .and_then(|io| io.raw_os_error()),
            Some(19),
            "-ENODEV is a real errno and must convert"
        );
        // -1 is ambiguous, not merely a sentinel: it is what drmtap_drm_fd() and a null
        // drmtap_open report, AND it is -EPERM, which the capture path can return for real
        // because it passes -errno through from drmModeGetFB2. Nothing in the integer separates
        // them, so this must not answer either one.
        assert!(
            Error { code: -1, message: "failed to open DRM device".into() }.io_error().is_none(),
            "-1 cannot be resolved to an errno or to a sentinel; do not guess"
        );
        assert!(Error { code: 0, message: String::new() }.io_error().is_none());
    }

    #[test]
    fn the_error_field_is_still_the_raw_code() {
        // `code` is a public field: this release only ADDS the accessor. Changing the field is a
        // breaking release, and this is what would notice it being changed anyway.
        let e = Error { code: -22, message: "x".into() };
        assert_eq!(e.code, -22);
        assert!(format!("{e}").starts_with("drmtap error -22:"));
    }
}
