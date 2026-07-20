# Changelog

Notable changes to libdrmtap. Loosely follows Keep a Changelog; the project uses
semantic versioning. The C library, the `libdrmtap-sys` crate and the meson
project share one version; the `libdrmtap` wrapper crate is versioned separately.

## [0.4.12] - 2026-07-20

### Security
- `drmtap_convert_dmabuf()` hardened at the untrusted convert boundary. A
  coverage-guided fuzzer found that a descriptor claiming a frame larger than
  the fd actually backs made the CPU fallback mmap and read past the fd,
  faulting the unprivileged converter with SIGBUS (a denial of service on a
  malformed IPC message). The fix requires the fd to be a genuine DMA-BUF (a
  non-dma-buf fd is rejected; only an immutable dma-buf is safe to mmap and read
  without a truncate-mid-read race) and bounds the read against the buffer size
  read with lseek (reliable across kernels, unlike fstat which reports 0 for a
  dma-buf before Linux 5.3). An oversized or non-dma-buf descriptor now returns
  -EINVAL instead of faulting.

### Added
- `fuzz/fuzz_convert.c` (+ build.sh, README): a libFuzzer target for the
  convert boundary, driving hostile descriptors over real udmabuf-backed
  dma-bufs. It survives tens of millions of runs clean after the fix.
- `DRMTAP_NO_EGL=1` forces the CPU deswizzle/convert path (for debugging and for
  the convert tests/fuzzer, which target the CPU-side untrusted handling).

## [0.4.11] - 2026-07-20

### Hardening
- `libdrmtap.so` now exports only the public API. A linker version script
  (`libdrmtap.map`, anonymous node so the public symbols stay unversioned) keeps
  every internal cross-module symbol local: `drmtap_set_error`,
  `drmtap_gpu_*_process`, `drmtap_helper_*`, `drmtap_ensure_buf`,
  `drmtap_convert_rgb16` and friends are no longer dlsym-able from the shared
  object the privileged service loads.
- The shared object gets the same exploit mitigations as the privileged helper:
  full RELRO (`-z relro -z now`), `_FORTIFY_SOURCE=2`, and an explicit
  stack-protector.
- `DRM_DEVICE` is honored only for an unprivileged process. A root capture
  service now ignores it and uses the explicit config path or KMS
  auto-detection, so an environment variable cannot redirect which device the
  privileged process opens.

### Changed
- Built as both a shared object (version-scripted, installed, the one loaded via
  dlopen) and a static archive (full symbol set). The white-box unit tests link
  the static archive; examples link the public shared library.

## [0.4.10] - 2026-07-20

### Added
- `drmtap_grab_desc()`: the exporter-side counterpart to
  `drmtap_convert_dmabuf()`. Does a zero-copy grab and fills a complete
  `drmtap_dmabuf_desc`, including the CCS auxiliary planes
  (num_planes/offsets/pitches) and the connector HDR state (eotf/max_nits) that
  `drmtap_grab()` and `drmtap_frame_info` do not carry. Needed to ship a
  compressed (Intel CCS) or HDR scanout across a process boundary without loss.
- `examples/split_capture.c`: end-to-end split-capture reference. Exporter and
  converter run in separate processes, the DMA-BUF fd is passed over SCM_RIGHTS.
  Verified on a real Intel i915 CCS scanout.

### Fixed
- `drmtap_grab_desc()` returns `-ENOTSUP` when the capture path yielded pixels
  but no transferable DMA-BUF (helper V2 fallback), instead of returning a
  descriptor the receiving process cannot convert.

## [0.4.9] - 2026-07-20

### Added
- Split-capture API for a privileged-export, unprivileged-convert boundary:
  `drmtap_open_render()` (render-node-only context, no KMS master or helper) and
  `drmtap_convert_dmabuf()` (detile and convert an exporter-supplied DMA-BUF to
  linear RGBA), plus the `drmtap_dmabuf_desc` descriptor.

### Changed
- libEGL and libGLESv2 are loaded lazily with dlopen on first conversion instead
  of being linked, so a process that only grabs never maps the GPU userspace
  stack. They no longer appear in the `.so` DT_NEEDED or the pkg-config
  `Requires.private`.
- The imported EGLImage is cached per KMS `fb_id` plus DMA-BUF inode identity
  (import once); steady-state conversion does no per-frame allocation.

### Fixed
- Fast-grab cache-hit paths restage the plane layout, so a CCS import never runs
  with stale plane metadata.
- `drmtap_convert_dmabuf()` validates the untrusted descriptor and rejects a
  stride narrower than `width * bytes-per-pixel`, closing an out-of-bounds
  read/write on the convert boundary.

## [0.4.8] - 2026-07-15

### Fixed
- The per-thread EGL detile context leaked on `drmtap_close()`, causing OOM under
  a reconnect flap. It is now freed on close and at thread exit.
- Wrong-CRTC selection: a `crtc_id == 0` / unbound connector was offered and
  auto-selected the primary. Added an atomic CRTC_ID fallback and inactive-output
  filtering.
- Added a rapid-rebuild demotion guard.

## [0.4.7] - 2026-07-13

### Fixed
- amdgpu EGL detile fix, privileged-helper hardening, and a batch of full-audit
  fixes.

[0.4.12]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.12
[0.4.11]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.11
[0.4.10]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.10
[0.4.9]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.9
[0.4.8]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.8
[0.4.7]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.7
