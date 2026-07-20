# Changelog

All notable changes to libdrmtap are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project follows
[semantic versioning](https://semver.org/). The C library, the `libdrmtap-sys`
crate and the meson project share one version line; the higher-level
`libdrmtap` wrapper crate is versioned separately.

## [0.4.11] - 2026-07-20

### Security / Hardening
- Export only the public ABI from `libdrmtap.so`. A linker version script
  (`libdrmtap.map`, an anonymous node so the public symbols stay unversioned)
  keeps every internal cross-module symbol local — `drmtap_set_error`,
  `drmtap_gpu_*_process`, `drmtap_helper_*`, `drmtap_ensure_buf`,
  `drmtap_convert_rgb16`, etc. are no longer `dlsym`-able from the shared object
  the privileged service loads. Only the documented public entry points are
  exported.

### Changed
- The library is now built as both a shared object (version-scripted, installed,
  the artifact that gets dlopen'd) and a static archive (full symbol set). The
  white-box unit tests link the static archive; examples link the public shared
  library.

## [0.4.10] - 2026-07-20

### Added
- `drmtap_grab_desc()` — the privileged-exporter counterpart to
  `drmtap_convert_dmabuf()`: does a zero-copy grab and fills a complete
  `drmtap_dmabuf_desc` including the CCS auxiliary planes
  (num_planes/offsets/pitches) and the connector HDR state (eotf/max_nits),
  which `drmtap_grab()` + `drmtap_frame_info` alone could not produce. Required
  to hand a compressed (Intel CCS) or HDR scanout across a process boundary
  losslessly.
- `examples/split_capture.c` — an end-to-end split-capture reference: exporter
  and converter run in separate processes with the DMA-BUF fd passed over
  SCM_RIGHTS. Verified on a real Intel i915 CCS scanout.

### Fixed
- `drmtap_grab_desc()` returns `-ENOTSUP` when the capture path produced pixels
  but no transferable DMA-BUF (helper V2 pixel fallback), instead of handing
  back a descriptor the receiving process cannot convert.

## [0.4.9] - 2026-07-20

### Added
- Split-capture API for a privileged-export / unprivileged-convert boundary:
  `drmtap_open_render()` (a render-node-only context, no KMS master or helper)
  and `drmtap_convert_dmabuf()` (detile/convert an exporter-supplied DMA-BUF to
  linear RGBA), with the `drmtap_dmabuf_desc` descriptor.

### Changed
- libEGL/libGLESv2 are `dlopen`'d lazily on first conversion instead of being
  linked, so a process that only grabs never maps the GPU userspace stack. They
  no longer appear in the `.so` DT_NEEDED nor in the pkg-config
  `Requires.private`.
- The imported EGLImage is cached per KMS `fb_id` plus the DMA-BUF inode
  identity (import-once); steady-state conversion performs no per-frame
  allocation.

### Fixed
- Fast-grab cache-hit paths restage the plane layout, so a CCS import never runs
  with stale plane metadata.
- `drmtap_convert_dmabuf()` validates the untrusted descriptor and rejects a
  stride narrower than `width * bytes-per-pixel`, preventing an out-of-bounds
  read/write on the convert boundary.

## [0.4.8] - 2026-07-15

### Fixed
- The per-thread EGL detile context was leaked on `drmtap_close()`, causing OOM
  under a reconnect flap — it is now released on close and at thread exit.
- Wrong-CRTC selection (a `crtc_id == 0` / unbound connector was offered and
  auto-selected the primary) — added an atomic CRTC_ID fallback and
  inactive-output filtering.
- Added a rapid-rebuild demotion guard.

## [0.4.7] - 2026-07-13

### Fixed
- amdgpu EGL detile fix, privileged-helper hardening, and a batch of
  full-audit fixes.

[0.4.11]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.11
[0.4.10]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.10
[0.4.9]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.9
[0.4.8]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.8
[0.4.7]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.7
