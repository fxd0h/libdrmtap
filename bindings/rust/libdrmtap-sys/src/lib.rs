/*
 * libdrmtap-sys — Raw FFI bindings for libdrmtap
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Hand-written FFI bindings matching include/drmtap.h
 * These are intentionally minimal — use libdrmtap-rs for safe wrappers.
 */

#![allow(non_camel_case_types)]

use std::os::raw::{c_char, c_int, c_void};

/// Opaque capture context
pub enum drmtap_ctx {}

/// Configuration for opening a capture context
#[repr(C)]
pub struct drmtap_config {
    pub device_path: *const c_char,
    pub crtc_id: u32,
    pub helper_path: *const c_char,
    pub debug: c_int,
}

/// Information about a connected display
#[repr(C)]
#[derive(Clone, Copy)]
pub struct drmtap_display {
    pub crtc_id: u32,
    pub connector_id: u32,
    pub name: [c_char; 32],
    pub width: u32,
    pub height: u32,
    pub refresh_hz: u32,
    pub active: c_int,
}

/// Captured frame data
#[repr(C)]
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

/// Cursor state
#[repr(C)]
pub struct drmtap_cursor_info {
    pub x: i32,
    pub y: i32,
    pub hot_x: i32,
    pub hot_y: i32,
    pub width: u32,
    pub height: u32,
    pub pixels: *mut u32,
    pub visible: c_int,
    pub _priv: *mut c_void,
}

/// Dirty rectangle from frame differencing
#[repr(C)]
pub struct drmtap_rect {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
}

extern "C" {
    // Version
    pub fn drmtap_version() -> c_int;

    // Context lifecycle
    pub fn drmtap_open(config: *const drmtap_config) -> *mut drmtap_ctx;
    pub fn drmtap_close(ctx: *mut drmtap_ctx);

    // Display enumeration
    pub fn drmtap_list_displays(
        ctx: *mut drmtap_ctx,
        displays: *mut drmtap_display,
        max_count: c_int,
    ) -> c_int;
    pub fn drmtap_displays_changed(ctx: *mut drmtap_ctx) -> c_int;

    // Frame capture
    pub fn drmtap_grab(ctx: *mut drmtap_ctx, frame: *mut drmtap_frame_info) -> c_int;
    pub fn drmtap_grab_mapped(ctx: *mut drmtap_ctx, frame: *mut drmtap_frame_info) -> c_int;
    pub fn drmtap_frame_release(ctx: *mut drmtap_ctx, frame: *mut drmtap_frame_info);

    // Cursor
    pub fn drmtap_get_cursor(ctx: *mut drmtap_ctx, cursor: *mut drmtap_cursor_info) -> c_int;
    pub fn drmtap_cursor_release(ctx: *mut drmtap_ctx, cursor: *mut drmtap_cursor_info);

    // Info
    pub fn drmtap_error(ctx: *const drmtap_ctx) -> *const c_char;
    pub fn drmtap_gpu_driver(ctx: *mut drmtap_ctx) -> *const c_char;

    // Pixel conversion
    pub fn drmtap_deswizzle(
        src: *const c_void,
        dst: *mut c_void,
        width: u32,
        height: u32,
        src_stride: u32,
        dst_stride: u32,
        modifier: u64,
    ) -> c_int;
    pub fn drmtap_convert_format(
        src: *const c_void,
        dst: *mut c_void,
        width: u32,
        height: u32,
        src_stride: u32,
        dst_stride: u32,
        src_format: u32,
        dst_format: u32,
    ) -> c_int;

    // Frame differencing
    pub fn drmtap_diff_frames(
        frame_a: *const c_void,
        frame_b: *const c_void,
        width: u32,
        height: u32,
        stride: u32,
        rects_out: *mut drmtap_rect,
        max_rects: c_int,
        tile_size: c_int,
    ) -> c_int;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_version() {
        let v = unsafe { drmtap_version() };
        assert_eq!(v, 0x000100); // 0.1.0
    }

    #[test]
    fn test_config_size() {
        // Verify struct sizes are reasonable
        assert!(std::mem::size_of::<drmtap_config>() > 0);
        assert!(std::mem::size_of::<drmtap_display>() > 0);
        assert!(std::mem::size_of::<drmtap_frame_info>() > 0);
        assert!(std::mem::size_of::<drmtap_cursor_info>() > 0);
    }
}
