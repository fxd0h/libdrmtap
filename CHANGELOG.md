# Changelog

Notable changes to libdrmtap. Loosely follows Keep a Changelog; the project uses
semantic versioning. The C library, the `libdrmtap-sys` crate and the meson
project share one version; the `libdrmtap` wrapper crate is versioned separately.

## [0.4.15] - 2026-07-23

### Fixed

- A CCS-compressed (or otherwise undecodable) scanout reaching the CPU deswizzle
  path no longer returns the raw compressed bytes relabelled linear as a valid
  frame. Two defects combined: gpu_auto_process suppressed the deswizzle -ENOTSUP,
  set the modifier to linear and returned success; and do_grab (the body of both
  drmtap_grab and drmtap_grab_mapped) discarded gpu_auto_process return value at its
  direct-mmap, helper-V2 and helper-V3 call sites. Either alone forwarded a corrupt
  frame instead of failing over. gpu_auto_process now returns -ENOTSUP, and do_grab
  propagates a non-zero process result (releasing the frame first) at all three
  sites, so the caller ends the stream and falls back. The symmetric case is closed
  too: a real (non-linear, non-INVALID) tiling modifier on a driver with no CPU
  deswizzle and no working EGL now also returns -ENOTSUP instead of the raw tiled
  bytes relabelled linear. do_grab error exits are uniform (every path leaves the
  frame owning nothing), so a defensive double-release is a harmless no-op.
- The cursor DMA-BUF read is now bracketed with the DMA_BUF_IOCTL_SYNC START/END
  pair the main frame path already uses, in both the direct path and the privileged
  helper. DMA-BUF CPU access is not guaranteed coherent, so a non-coherent exporter
  (ARM / Tegra / Jetson) could return stale or partially updated cursor pixels, which
  a content-hash consumer then suppressed and the remote cursor froze.
- The EGL XRGB8888 import-retry is now restricted to a single-plane 8-bit RGB source
  (XRGB8888 / ARGB8888). Reinterpreting anything else as XRGB8888 returned corruption
  as success: a BGR order swapped red and blue, and a multi-plane / CCS-compressed
  buffer dropped its auxiliary planes so the shader sampled compressed data. Such an
  import now fails so the caller falls back, instead of retrying into a wrong layout.
  The final no-modifier retry is now also gated to a LINEAR source: dropping a real
  tiling modifier made EGL sample tiled bytes as linear and return corruption as
  success. (Extends the 0.4.14 high-bit-depth and plane-0-offset retry fixes.)

## [0.4.14] - 2026-07-22

### Fixed

- Drop DRM master after opening the card node when the caller holds CAP_SYS_ADMIN,
  in both the library and the privileged helper. libdrmtap only reads scanout and
  never modesets, but if a process holding CAP_SYS_ADMIN opened the node while no
  client held master (e.g. an unattended service started at boot before the
  compositor) the kernel granted it implicitly, which then blocked a compositor from
  reacquiring master on a VT switch and left the display black or frozen. Master is
  now dropped defensively there. The gate is the capability, checked with the capget
  syscall, not the uid: a caller without CAP_SYS_ADMIN (including a uid-0 process
  that dropped it) relies on the implicit master for drmModeGetFB2 to return
  framebuffer handles and must keep it.
- Honor the DRM_MODE_FB_MODIFIERS flag before trusting the framebuffer modifier, on
  every grab path (the primary grab, the fast-capture cache and the privileged
  helper). drmModeGetFB2 leaves the modifier field undefined when the flag is clear,
  so a driver that tiles the scanout without advertising a modifier was imported as
  if linear and produced corruption (the recurring 10-bit XR30 class). When the flag
  is clear the modifier is now reported as DRM_FORMAT_MOD_INVALID: with a DMA-BUF and
  EGL the import omits the modifier attribute and lets the driver infer the real
  layout; when EGL cannot run (no DMA-BUF fd, or EGL unavailable) an unknown layout is
  treated as linear and reduced from the raw mapping, which keeps 10-bit and 16-bit
  scanouts correct instead of diverting them into the CPU deswizzle.
- The EGL import no longer falls back to reinterpreting a high-bit-depth scanout as
  XRGB8888. When the native fourcc import failed, the retry forced XRGB8888 while
  keeping the source stride, which sampled a 10-bit (XR30 family) or 16-bit (XR48
  family) buffer at the wrong bit depth and returned it as a valid frame. A
  high-bit-depth import that fails now fails cleanly so the caller reduces the real
  bit depth from the raw mapping on the CPU; the XRGB8888 retry stays for genuine
  8-bit sources whose exact fourcc a driver does not recognize.
- The privileged helper leaked a GEM handle on every grab and every cursor poll:
  drmModeGetFB2 mints a fresh handle the caller owns, and no path closed it, so the
  long-running root helper pinned one buffer object per frame until exhaustion. Both
  helper paths and the direct cursor path in the library now close the handle on every
  return. (The main grab path in the library already closed its handles.)
- The EGL convert path no longer returns a stale or uninitialized frame as success
  after GPU context loss. A failed eglMakeCurrent is now fatal, and a GL error across
  the render and readback (notably a context reset) fails the convert instead of
  handing back whatever was in the readback buffer.
- HDR displays are now tone-mapped on Wayland direct capture. Both HDR-metadata reads
  (the library and the helper) mapped a CRTC to its connector through the legacy
  encoder link, which reads 0 under atomic KMS, so a compositor-managed HDR connector
  never matched and never tone-mapped. They now match on the atomic CRTC_ID property.
- Half-float FP16 scanouts (XRGB16161616F and siblings) are reduced correctly on the
  CPU fallback instead of being misread as 8-bit and returned as a corrupt frame. The
  new converter decodes each half as linear light and re-encodes through the sRGB
  OETF (HDR highlights clip).
- Monitor hotplug and modeset are now detected. drmtap_displays_changed compared the
  connector and CRTC object counts, which are fixed by the GPU hardware, so a monitor
  plugged or unplugged on an existing connector never registered. It now hashes the
  connection state and bound CRTC of each connector.
- The XRGB8888 EGL import retries honor the plane-0 offset from the framebuffer
  instead of hardcoding 0, so a scanout whose pixels start at a non-zero BO offset is
  not imported shifted.
- drmtap_convert_format rejects a source or destination stride narrower than
  width times four, matching drmtap_convert_rgb16, closing an out-of-bounds row
  access on a malformed geometry.
- 10-bit BGR scanouts (XBGR2101010 / ABGR2101010, the XB30 family) are now reduced
  and tone-mapped on the CPU fallback. Both the SDR reduction and the HDR tone-map
  handled only the RGB 10-bit order (XR30 / AR30), so a 10-bit BGR scanout fell
  through unreduced and was returned as a corrupt frame. The channel order is now
  selected from the fourcc.
- The fast-capture path falls back to EGL when a scanout cannot be CPU-mapped. A
  tiled scanout on some drivers (amdgpu GFX9+, discrete VRAM, nvidia) refuses an mmap
  of the exported DMA-BUF; grab_mapped_fast returned -ENOMEM there instead of
  EGL-detiling the fd the way the primary grab does. It now takes that fallback (no
  CPU mapping is cached for such a frame; the fd is re-exported each grab). Validated
  on Intel via a DRMTAP_FORCE_MMAP_FAIL test hook that drops the CPU mapping so the
  fallback runs on any EGL-capable GPU.
- The grab_mapped_fast doc no longer claims it returns 1 on an unchanged framebuffer.
  It always re-reads the current scanout and returns a fresh frame, because a
  compositor can render into the same framebuffer without a page flip, so an
  fb_id-unchanged skip would miss content updates. The identity assumption (a stable
  fb_id denotes the same buffer between captures) is now documented.

### Security

- drmtap_convert_dmabuf now validates the untrusted fd (genuine dma-buf plus the
  offset-and-size bound added in 0.4.12) BEFORE the EGL import, not only in the CPU
  fallback. The EGL path runs first and is the one the intended unprivileged converter
  takes, so a hostile descriptor claiming a frame larger than the dma-buf backs
  previously reached eglCreateImage unbounded; it is now rejected up front.
- The helper seccomp filter restricts ioctl to the DRM ioctl type instead of allowing
  every ioctl. The helper issues nothing but DRM ioctls on its one DRM fd, so a
  memory-corrupted helper can no longer reach an unrelated ioctl (terminal control,
  console injection). Matching the type byte stays robust across drivers without
  risking a KILL on a DRM command not individually listed.
- The privileged helper no longer honors DRM_DEVICE from the environment. It takes the
  device solely from the library-selected, vetted argv path (the library already
  ignores DRM_DEVICE when privileged, since 0.4.11).

### Performance

- The HDR-metadata read no longer forces a hardware connector probe on every captured
  frame. Both the library and the helper now use drmModeGetConnectorCurrent, which
  reads cached kernel state instead of re-probing the connector each frame.
- The 16-bit HDR (PQ) reduction uses a precomputed 65536-entry lookup table instead of
  calling pow twice per channel per pixel (tens of millions of pow calls per 4K frame).
- The privileged helper caches which planes are PRIMARY instead of re-reading each
  plane's static type property (an object-properties fetch plus a per-property lookup)
  on every captured frame.

## [0.4.13] - 2026-07-20

### Hardening

- The privileged helper command frame now carries a magic and a protocol
  version. The helper validates the magic, version, length and command type of
  every frame at one gate before dispatch, and closes the connection on a
  mismatch instead of replying, so a helper and a library built from different
  releases fail closed rather than misreading each other while the helper holds
  CAP_SYS_ADMIN. The command frame moved to one shared header (wire.h) instead of
  being duplicated on each side.

### Changed

- SECURITY.md now documents the split-capture convert trust boundary (the
  converter treats the exporter-supplied descriptor and fd as untrusted IPC
  input) and the new command-frame header.

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

[0.4.15]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.15
[0.4.14]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.14
[0.4.13]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.13
[0.4.12]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.12
[0.4.11]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.11
[0.4.10]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.10
[0.4.9]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.9
[0.4.8]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.8
[0.4.7]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.7
