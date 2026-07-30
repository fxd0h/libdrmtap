# Changelog

Notable changes to libdrmtap. Loosely follows Keep a Changelog; the project uses
semantic versioning. The C library, the meson project, the `libdrmtap-sys` crate and
the `libdrmtap` wrapper crate all share ONE version (since 0.5.0; before that the
wrapper had its own 0.3.x line).

## [0.5.2] - 2026-07-30

Documentation only; no code changed. Published so the crates.io pages carry the
right text: the `libdrmtap` crate ships its README, and 0.5.1 went out with that
README still saying it pulls in `libdrmtap-sys` 0.4.14.

The version references in prose are now written so they stop rotting. Copy-pasteable
dependency snippets use a `"0.5"` caret range, which stays valid across every patch;
sentences that did not need a number no longer carry one (the crates.io badge is
dynamic and reports it); dated status tables say `0.5.x`; and AGENTS.md points at
`include/drmtap.h` as the canonical number instead of keeping a copy that goes stale
on every release. `tools/check-version.sh` still verifies the six code sites exactly
-- only the prose stopped duplicating them.

Also drops the CodeRabbit reviews badge: its shields.io endpoint answers "provider or
repo not found" for every repository, including CodeRabbit's own, so it rendered an
error to every visitor.

## [0.5.1] - 2026-07-30

A packaging release: no C code changed from 0.5.0. It exists so the two published
crates carry the SAME number as the library, which 0.5.0 could not.

0.5.0 shipped `libdrmtap-sys` while the `libdrmtap` wrapper was still on its own
0.3.x track, and a published wrapper depending on `^0.4.7` cannot resolve to a 0.5.x
`-sys` at all -- so a crates.io user of the wrapper would have kept a library with
neither the scanout-width fix nor the dimension bounds, silently. The wrapper is on
the shared version line now (0.3.4 -> 0.5.1), `tools/set-version.sh` stamps all six
sites, and `tools/check-version.sh` VERIFIES the wrapper version instead of
filtering it out of its scan -- that filter is what let this hide, since the one
number that could be wrong was the one excluded from the check.

Both crates are published at 0.5.1 together, so `libdrmtap 0.5.1` and
`libdrmtap-sys 0.5.1` are the pair to use. `libdrmtap-sys` 0.5.0 remains on
crates.io and is functionally identical; nothing needs to move off it except a
wrapper user, who could never reach it in the first place.

No effect on rustdesk, which dlopens `libdrmtap.so.0` and deliberately depends on
neither crate.

## [0.5.0] - 2026-07-30

A MINOR bump, not a patch, for one reason: the scanout-width fix below changes the
SEMANTICS of an existing call. Any consumer reading drmtap_frame_info.width can now
get a different number for the same hardware. Nothing was removed and the soname is
unchanged (still libdrmtap.so.0), so a dlopen-by-soname consumer keeps loading it --
which is exactly why the version has to say so: a consumer built against 0.4.x
semantics would otherwise pick up different widths with no signal at all. The new
entry point is additive and would not on its own have justified more than a patch.

### Added

- drmtap_set_output_buffer(ctx, dst, len): have conversions write into a
  caller-owned buffer instead of a library-owned one, so a consumer that must end
  up with the pixels in specific memory (a memfd it already shares downstream, a
  pre-registered upload buffer) stops paying a full-frame memcpy per frame -- at
  2560x1440 that was 14.7 MB a frame for nothing (issue #36). Nothing required the
  copy: the EGL detile ends in a glReadPixels and the CPU converters take a
  destination pointer, and both write wherever they are pointed.
  Applies to every path that materializes pixels -- EGL detile, CPU deswizzle,
  10/16-bit and HDR reductions, padded-linear repack -- so a caller does not have
  to know which one ran, and on an unprivileged drmtap_open_render() context too,
  so the converting half of the split can write straight into the consumer's
  buffer. It does NOT apply where there is nothing to materialize: a linear 8-bit
  scanout is reported where the pixels already are -- the mapped scanout on the
  direct path, the library receive buffer on the privileged-helper pixel path -- so
  no conversion destination is chosen. A frame that would not fit is REFUSED with
  -ENOSPC, leaving the buffer untouched and naming the required size through
  drmtap_error(), rather than writing short.

### Security

- Framebuffer DIMENSIONS are now bounded at every entry point, not just
  `stride * height`. validate_fb_size() never constrained width, yet every converted
  output is sized `width * height * 4` -- a product that can wrap size_t and hand a
  large write a small destination. Reachable with a width from the helper wire or
  an IPC-supplied descriptor. Each dimension is now bounded before anything
  multiplies it (so the product cannot wrap even where size_t is 32-bit) and the
  pixel count is bounded after. The frame cap also applies to a caller-supplied
  output buffer, so owning a large mapping cannot unlock a frame larger than this
  library handles. Verified under ASan and UBSan against a real tiled scanout,
  with guard bytes either side of the destination.
- The privileged-helper wire is now bounded on stride-covers-width, which the two
  existing checks did not imply. validate_fb_size bounds `stride * height` and
  validate_fb_dims bounds the dimensions, but a wire frame claiming
  `width * bpp > stride` still reached the per-pixel CPU converters, which read
  `y * stride + width * bpp` per row with no size argument, so the last rows ran off
  the end of the receive buffer (a heap over-read). drmtap_convert_dmabuf already
  applied this check to its IPC descriptor; the two trust boundaries are bounded
  alike now.

### Fixed

- A grab reported the width of the framebuffer OBJECT instead of the width the
  CRTC actually scans out, so a display whose scanout fb is padded wider than its
  mode could not be captured at all by a consumer that had enumerated the mode.
  Measured on an Apple T2 MacBook: the Touch Bar card (appletbdrm) drives a
  60x2170 mode from a 64x2170 framebuffer with a 256-byte pitch, and those four
  columns are alignment padding that is never scanned out. Every frame came back
  64 wide against a display advertised as 60, which rustdesk read as "this
  display never matched its advertised geometry"; it rejected the frames,
  demoted the display to PipeWire, and the client sat on "waiting for image".
  Reported geometry is now narrowed to what the primary plane actually reads, and
  the mode is only the cheap gate that says "worth looking closer". The plane rect
  is what decides, because a framebuffer wider than the mode looks IDENTICAL to
  pitch padding whether it is padding or a plane that genuinely downscales, and
  getting that wrong returns part of the image as if it were the whole screen. On
  the measured machine the plane reports SRC 60x2170 onto a 60x2170 CRTC rect, so
  the four columns are provably not scanned out. A pitch test cannot substitute:
  `fb_width * bpp == pitches[0]` holds for a 3840-wide downscaled framebuffer exactly
  as it does for the padded 64-wide one.
  It only ever SHRINKS, and only the width, and every refusal is a case where the
  framebuffer width is the right answer: a scaling plane (the whole fb is
  displayed, just smaller), a plane reading from a non-zero origin (several heads
  out of one framebuffer, which would need an offset crop that the frozen
  drmtap_dmabuf_desc has nowhere to carry), a TILED layout (the CPU deswizzle
  derives its tile grid from the width and uses it to address the SOURCE, so a
  narrowed width there mis-addresses every tile row after the first and silently
  mangles the image), and an unreadable plane rect -- which is NOT the same as "no
  scaling" and is not treated as one.
  Reading the plane rect needs DRM_CLIENT_CAP_ATOMIC, since the SRC and CRTC
  properties are hidden from a non-atomic client (measured: the same plane reports
  SRC_W with the cap and nothing without it). The cap is requested LAZILY, only
  once a framebuffer has turned out to be wider than its mode, so the common path
  never touches it; it is per-fd, needs no privilege and no DRM master, no atomic
  commit is ever issued, and no other client is affected.
  Buffer-size arithmetic is unchanged -- the mapping is still of the whole padded
  fb, only the reported width narrows and the caller reads rows at the unchanged
  stride. No ABI change: the decision is an internal function, the exported symbol
  set is untouched.

- drmtap_grab_mapped_fast() said nothing useful when it could not read the
  scanout (issue #36, a Radeon Vega 64 on amdgpu where the tiled VRAM scanout
  refuses a CPU mmap). Three separate silences: the no-mapping-and-no-EGL exit
  returned -ENOMEM without ever calling drmtap_set_error, so drmtap_error() was
  empty and the only symptom was a wall of cache-miss lines; the per-frame miss
  line said "cold start" on a device where every frame is a miss BY DESIGN,
  because the EGL fd fallback has no CPU mapping to cache; and the fallback
  itself was never announced. Now the reason is logged once with the errno, the
  per-frame line distinguishes an uncachable scanout from a genuine cold start,
  and the no-EGL exit names the cause and the remedy. The failing mmap is no
  longer retried per frame either: whether a scanout BO is CPU-mappable is a
  property of the driver and the placement, so a context learns it once.
  The capture itself has worked since 0.4.14, which added the mmap-to-EGL
  fallback; on 0.4.13 that path returned -ENOMEM on every frame, which is why
  the reporter saw black.

### Changed

- The `libdrmtap` wrapper crate joins the single version line instead of keeping its
  own 0.3.x track, going 0.3.4 -> 0.5.0. The separate track was not free: a MINOR
  bump of `libdrmtap-sys` leaves a published wrapper pinned to `^0.4.7`, a range that
  cannot reach 0.5.0, so every crates.io user of the wrapper would have silently kept
  a library without the scanout-width fix or the dimension bounds in it, with nothing
  anywhere reporting a problem. Lockstep costs a wrapper republish on a minor bump
  only -- its dependency is a caret range, so a PATCH of `libdrmtap-sys` is still
  picked up with no republish. tools/set-version.sh stamps all six sites now and
  tools/check-version.sh verifies the wrapper version instead of filtering it out.
- New meson option `egl` (feature, default `auto`, so the historical behaviour is
  unchanged). `enabled` hard-fails when the egl/glesv2 pkg-config files or
  headers are missing, instead of quietly building the CPU-only stub. CI now
  passes `-Degl=enabled` on every C build and installs libegl-dev plus
  libgles2-mesa-dev, because it had neither: every C job was building and testing
  the stub, so the GPU detiling backend that real deployments run had no
  automated coverage at all, and nothing would have reported it. The release job
  additionally asserts the built .so carries the dlopen target name and an EGL
  entry point, since the backend is reached by lazy dlopen and therefore leaves
  no DT_NEEDED entry for the obvious ELF check to find.

## [0.4.15] - 2026-07-24

### Added

- drmtap_list_devices(): enumerate every DRM device with KMS resources, with its
  card node, render node, driver and active-display count. A context is bound to
  ONE device, so drmtap_list_displays() could only ever advertise the displays of
  the card drmtap_open() settled on, and the monitors of every other GPU were
  invisible. This is the discovery step a multi-GPU consumer needs to open one
  context per device. Connector state is not re-probed, so enumerating does not
  disturb a live display. The drmtap_device layout is frozen for the same reason
  drmtap_dmabuf_desc is.
- drmtap_render_node(): the render node of the DRM device a context is bound to.
  On a split deployment the privileged exporter calls it on its capture context
  and hands the path to the unprivileged converter, so drmtap_open_render()
  binds to the GPU that exported the frame rather than to whichever node
  auto-selection happened to reach. Deliberately a separate accessor and not a
  new field in drmtap_dmabuf_desc: that struct is written by drmtap_grab_desc()
  into caller-owned storage, so growing it would corrupt a consumer built
  against an older header but running against a newer .so, which is exactly the
  dlopen-by-soname deployment this library targets. The descriptor layout stays
  frozen, and a consumer that dlopens can treat an absent symbol as an older
  library.

### Fixed

- Device auto-detection now picks the GPU driving the MOST displays instead of
  the first card with any active CRTC, which was really just "lowest minor
  wins". Loading vkms next to a real GPU demonstrates the old behaviour: vkms
  registers as card0 with a single 1024x768 virtual output, so auto-detect
  captured that and not the three real monitors on card1. It stays a heuristic
  for the single-context case; a caller that wants every display enumerates with
  drmtap_list_devices().
- drmtap_open_render(NULL) no longer takes the first openable render node, which
  was the wrong device on a multi-GPU host: the scanout is exported by the card
  that drives the display, and importing it into another vendor render node can
  fail permanently on an incompatible tiling modifier. Auto-selection now
  prefers the render node of a card with a connected and enabled connector, then
  a card with a connected connector, and only then falls back to the historical
  first-openable scan. The ranking is read from sysfs so it needs no rights on
  the KMS cards, which the unprivileged converter may not have. Verified on a
  Jetson Orin, where card1 is tegra with no connectors and card2 is nvidia-drm
  driving the connected DP-1: selection moved from renderD128 to renderD129, the
  node that actually exports the scanout.
- The EGL display is now resolved and cached per DRM device instead of once per
  process. A single process-wide display is bound to whichever GPU converted
  first, so on a multi-GPU host it kept serving imports of scanouts exported by
  the other card, which can fail permanently. A capture thread that moves to a
  different device now tears down and rebuilds its GL context rather than
  sampling another GPU display, and EGL availability is cached per device too, so
  a compute GPU without EGL next to one with it is answered correctly.
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

[0.5.2]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.5.2
[0.5.1]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.5.1
[0.5.0]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.5.0
[0.4.15]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.15
[0.4.14]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.14
[0.4.13]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.13
[0.4.12]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.12
[0.4.11]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.11
[0.4.10]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.10
[0.4.9]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.9
[0.4.8]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.8
[0.4.7]: https://github.com/fxd0h/libdrmtap/releases/tag/v0.4.7
