# 🤖 Agent Guidelines

> This document is designed to be read by both AI coding agents and human developers.
> It defines how to work on libdrmtap in a way that is clear, predictable, and reviewable.

---

## Quick Context for Agents

**What is libdrmtap?** A C library for capturing the Linux screen by reading the active scanout framebuffer directly via DRM/KMS. No Wayland portal, no PipeWire, no consent dialog — works under X11, Wayland, headless, and VMs.

**Key files to read first:**
- [`docs/research/05_api_and_architecture.md`](docs/research/05_api_and_architecture.md) — API design and architecture
- [`docs/research/02_drm_kms_mechanism.md`](docs/research/02_drm_kms_mechanism.md) — How DRM/KMS capture works
- [`docs/research/08_reframe_egl_analysis.md`](docs/research/08_reframe_egl_analysis.md) — EGL GPU-universal detiling (the primary detile path)
- [`docs/research/06_github_issues_analysis.md`](docs/research/06_github_issues_analysis.md) — Known gotchas

**Build system:** Meson  
**Language:** C11  
**License:** MIT  
**Tests:** unit (no hardware) + integration via VKMS (Virtual KMS) or a real GPU  
**Version:** C library `0.4.3` · Rust crates `libdrmtap-sys 0.4.3` / `libdrmtap 0.3.2`  

---

## Code Style

### C Style Rules

```c
// ✅ CORRECT: snake_case, 4 spaces, braces on same line
int drmtap_grab_mapped(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    if (!ctx || !frame) {
        return -EINVAL;
    }
    
    int ret = refresh_plane_state(ctx);
    if (ret < 0) {
        set_error(ctx, "Failed to refresh plane: %s", strerror(-ret));
        return ret;
    }
    
    return 0;
}

// ❌ WRONG: camelCase, tabs, K&R braces
int drmtapGrabMapped(DrmtapCtx *ctx, DrmtapFrameInfo *frame)
{
	if (!ctx || !frame) return -EINVAL;
}
```

| Rule | Convention |
|---|---|
| Indent | 4 spaces, never tabs |
| Naming: functions | `snake_case`, prefixed `drmtap_` for public API |
| Naming: variables | `snake_case` |
| Naming: macros/constants | `UPPER_SNAKE_CASE`, prefixed `DRMTAP_` |
| Naming: types | `snake_case_t` for internal, `drmtap_*` for public |
| Braces | Same line (1TBS style) |
| Line length | 100 chars soft limit, 120 hard limit |
| Comments | `//` for single-line, `/* */` for multi-line |
| Header guards | `#ifndef DRMTAP_MODULE_H` / `#define DRMTAP_MODULE_H` |
| Error handling | Return negative errno values, never abort |
| Memory | Always check malloc returns, always free in cleanup |

### File Organization

```
// Every .c and .h file MUST start with this header block:

/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file drm_enumerate.c
 * @brief Plane, CRTC, and connector enumeration via DRM/KMS
 */

// After the header block, the file follows this structure:

// 1. Includes (grouped: system, library, project)
#include <stdio.h>        // system
#include <xf86drm.h>      // library
#include "drmtap.h"       // project

// 2. Private types and constants
#define MAX_PLANES 32

// 3. Static (private) functions
static int find_primary_plane(int drm_fd, uint32_t *plane_id) { ... }

// 4. Public functions (matching header declaration order)
int drmtap_list_displays(drmtap_ctx *ctx, ...) { ... }
```

### Doxygen Documentation

```c
// Public API functions in drmtap.h use Doxygen comments:

/**
 * @brief Capture a frame with mapped pixel data.
 *
 * Returns a pointer to linear RGBA pixel data in frame->data.
 * Handles GPU tiling → linear conversion automatically.
 *
 * @param ctx   Capture context from drmtap_open()
 * @param frame Output frame info (caller-allocated)
 * @return 0 on success, negative errno on error
 * @retval -EACCES Helper not found or not configured
 * @retval -ENODEV Display disconnected or CRTC inactive
 */
int drmtap_grab_mapped(drmtap_ctx *ctx, drmtap_frame_info *frame);

// Internal static functions use simple // comments:
// Refresh plane fb_id from the kernel (never cache!)
static int refresh_plane_state(drmtap_ctx *ctx) { ... }
```

### Commit Messages

```
component: short description (imperative mood)

Longer explanation of what and why (not how).
Reference issues with #NNN.

Examples:
  grab: refresh plane fb_id on every frame
  helper: export scanout as DMA-BUF and pass fd via SCM_RIGHTS
  gpu-egl: detile CCS framebuffers via EGLImage import
  tests: add vkms enumeration test
  docs: update GPU compatibility table
```

Prefixes: `grab`, `enumerate`, `formats`, `cursor`, `helper`, `gpu-egl`, `gpu-intel`, `gpu-amd`, `gpu-nvidia`, `gpu-generic`, `tests`, `docs`, `build`, `ci`.

---

## Success Criteria

Every change must satisfy ALL of these before merge:

### 1. It compiles
```bash
meson setup build && meson compile -C build
# Zero warnings with -Wall -Wextra -Werror
```

### 2. It doesn't break existing tests
```bash
meson test -C build
# All existing tests pass
```

### 3. New code has tests (when possible)
- Any new function should have a corresponding test
- Exception: GPU-specific code that requires real hardware (document this)

### 4. It's documented
- Public API changes → update `include/drmtap.h` comments
- New gotchas discovered → update `docs/research/06_github_issues_analysis.md`
- Architecture changes → update `docs/research/05_api_and_architecture.md`

### 5. It handles errors
- Never crash, never abort
- Return error codes (negative errno)
- Set human-readable error via `set_error(ctx, "...")`
- Clean up resources on error paths (goto cleanup pattern)

```c
// ✅ CORRECT error handling pattern
int drmtap_grab(drmtap_ctx *ctx, drmtap_frame_info *frame) {
    int ret;
    int prime_fd = -1;
    void *mapped = MAP_FAILED;
    
    ret = get_framebuffer(ctx, &prime_fd);
    if (ret < 0) {
        goto cleanup;
    }
    
    mapped = mmap(NULL, size, PROT_READ, MAP_SHARED, prime_fd, 0);
    if (mapped == MAP_FAILED) {
        ret = -errno;
        set_error(ctx, "mmap failed: %s", strerror(errno));
        goto cleanup;
    }
    
    // ... use mapped ...
    ret = 0;

cleanup:
    if (mapped != MAP_FAILED) munmap(mapped, size);
    if (prime_fd >= 0) close(prime_fd);
    return ret;
}
```

---

## Testing

### Test Structure

```
tests/
├── test_enumerate.c      # Plane/CRTC enumeration
├── test_formats.c        # Pixel format detection and conversion
├── test_capture.c        # Frame capture (requires vkms)
├── test_helper.c         # Privileged helper IPC
└── test_deswizzle.c      # Tiling format conversion (unit, no GPU)
```

### Running Tests

```bash
# Unit tests (no hardware needed): formats, deswizzle, helper
meson test -C build --suite unit

# Integration tests (need a DRM device): enumerate, capture
# vkms gives a synthetic scanout for CI-friendly testing
sudo modprobe vkms
DRM_DEVICE=/dev/dri/card1 meson test -C build --suite integration

# Same integration suite against real hardware (Intel/Nvidia/AMD/virtio)
DRM_DEVICE=/dev/dri/card0 meson test -C build --suite integration
```

There are two suites only: `unit` and `integration`. There is no separate
`gpu` suite — point the integration suite at a real GPU via `DRM_DEVICE`.

### Writing Tests

```c
// Tests use simple assert-based validation
// No external test framework needed

#include <assert.h>
#include <stdio.h>
#include "drmtap.h"

static void test_open_close(void) {
    drmtap_config cfg = {0};
    drmtap_ctx *ctx = drmtap_open(&cfg);
    assert(ctx != NULL);
    
    const char *driver = drmtap_gpu_driver(ctx);
    assert(driver != NULL);
    printf("  driver: %s\n", driver);
    
    drmtap_close(ctx);
    printf("  PASS: test_open_close\n");
}

int main(void) {
    printf("Running enumeration tests...\n");
    test_open_close();
    printf("All tests passed!\n");
    return 0;
}
```

---

## Pull Request Format

### PR Title
```
[component] Short description
```
Examples:
- `[grab] Refresh fb_id on every frame to fix double-buffering skip`
- `[gpu-amd] Add SDMA copy fallback for tiled framebuffers`
- `[docs] Document Intel CCS workaround`

### PR Description Template

```markdown
## What

Brief description of what this PR does.

## Why

What problem does it solve? Link to issue if applicable.

## How

High-level explanation of the approach.

## Testing

- [ ] Compiles with `meson compile -C build` (zero warnings)
- [ ] All existing tests pass
- [ ] New tests added (if applicable)
- [ ] Tested on: [hardware/driver/distro]

## Checklist

- [ ] Follows code style (AGENTS.md)
- [ ] Error handling uses goto-cleanup pattern
- [ ] Public API documented in header comments
- [ ] No memory leaks (checked cleanup paths)

## AI Disclosure (optional)

If AI tools were used, briefly mention which and for what.
This is optional and has no effect on PR review.
```

---

## Agent-Specific Instructions

### Before Starting Work

1. **Read the research docs** — especially `05_api_and_architecture.md` and `06_github_issues_analysis.md`
2. **Check the gotcha checklist** in `06_github_issues_analysis.md` — don't re-introduce known bugs
3. **Understand the architecture layers** — don't mix concerns between grab/enumerate/convert/helper

### When Writing Code

1. **Follow the error handling pattern** — `goto cleanup`, never abort
2. **Never cache `fb_id`** — always refresh via `drmModeGetPlane()` each frame
3. **Detect `handles[0] == 0`** — this means CAP_SYS_ADMIN is missing, trigger helper
4. **Use Prime path (not GEM_FLINK)** — GEM_FLINK doesn't work with vkms
5. **Check modifier for tiling** — `DRM_FORMAT_MOD_LINEAR` means no deswizzle needed
6. **EGL is the primary detile path** — tiled/compressed FBs (Intel CCS, AMD, Nvidia
   block-linear, vendor modifiers) go through `gpu_egl.c` (import DMA-BUF as EGLImage,
   draw, `glReadPixels` to linear RGBA). CPU deswizzle is only a fallback for some formats.
7. **HDR10 is tone-mapped to SDR** — when the connector reports HDR
   (`HDR_OUTPUT_METADATA`, PQ), `AR30`/`XR30` and 16-bit `XR48`/`AR48`/`XB48`/`AB48`
   scanouts are PQ-decoded, BT.2020 → BT.709 gamut-mapped, tone-mapped and
   sRGB-encoded to 8-bit, in both the CPU and the EGL (tiled) paths. Plain SDR
   10-bit gets a straight bit-depth reduction. `P010` (overlay-video YUV) and HLG
   are **not** tone-mapped — don't claim those as HDR-supported.

### When Writing Tests

1. **vkms is LINEAR only** — don't test tiling/deswizzle against vkms
2. **vkms needs a compositor** — bare vkms has no active planes
3. **Use DRM_DEVICE env var** — never hardcode `/dev/dri/card0`

### When Writing Documentation

1. **English always**
2. **Include date and sources** in research docs
3. **Update the gotcha checklist** when discovering new issues
4. **Link to kernel docs** or upstream sources when referencing DRM APIs

### When Creating PRs

1. **One concern per PR** — don't mix GPU backends with API changes
2. **Include test results** — even if just "tested with vkms on Ubuntu 24.04"
3. **Reference the research docs** when your change is based on a finding

---

## Directory Structure Reference

```
libdrmtap/
├── AGENTS.md              ← YOU ARE HERE (agent + human guidelines)
├── README.md              ← Project overview
├── LICENSE                ← MIT
├── CONTRIBUTING.md        ← How to contribute
├── SECURITY.md            ← Threat model + helper hardening
├── meson.build            ← Build system (project version lives here)
├── include/
│   └── drmtap.h           ← Public API (the only public header)
├── src/
│   ├── drmtap.c           ← Main context management + lifecycle
│   ├── drmtap_internal.h  ← Internal shared declarations (NOT installed)
│   ├── drm_enumerate.c    ← Plane/CRTC/connector enumeration
│   ├── drm_grab.c         ← Scanout framebuffer capture (V3 zero-copy / V2 fallback)
│   ├── cursor.c           ← Hardware cursor plane: image, position, hotspot
│   ├── pixel_convert.c    ← CPU deswizzle + format conversion (fallback path)
│   ├── gpu_egl.c          ← GPU-universal EGL/GLES2 detiler (PRIMARY detile path)
│   ├── gpu_intel.c        ← Intel i915/xe tiling modifiers (CCS/X/Y-tiled)
│   ├── gpu_amd.c          ← AMD amdgpu tiling (implemented, not yet HW-verified)
│   ├── gpu_nvidia.c       ← Nvidia block-linear handling (incl. Tegra/Jetson)
│   ├── gpu_generic.c      ← Generic/VM linear backend (virtio-gpu, simple VMs)
│   └── privilege_helper.c ← Helper spawn + SCM_RIGHTS / DMA-BUF passing
├── helper/
│   └── drmtap-helper.c    ← Privileged helper binary (CAP_SYS_ADMIN, seccomp-hardened)
├── tests/
│   ├── test_enumerate.c    ← integration suite (needs DRM device)
│   ├── test_capture.c      ← integration suite (needs vkms or real GPU)
│   ├── test_formats.c      ← unit suite
│   ├── test_helper.c       ← unit suite
│   ├── test_deswizzle.c    ← unit suite
│   └── lsan.supp           ← LeakSanitizer suppressions
├── examples/
│   ├── screenshot.c        ← Capture one frame → PNG
│   ├── stream.c            ← Continuous capture
│   └── vnc_server.c        ← VNC demo (optional libvncserver)
├── bindings/
│   └── rust/               ← libdrmtap-sys (FFI, embeds C sources + helper) + libdrmtap (safe)
├── docs/
│   ├── AI_DEVELOPMENT.md   ← AI development philosophy
│   ├── README.md           ← Docs index
│   └── research/           ← 9 technical research documents (00–08)
└── .github/
    ├── workflows/ci.yml    ← Build & Test (Ubuntu 22.04/24.04), Rust crate, cppcheck, CodeQL
    ├── ISSUE_TEMPLATE/
    │   ├── bug_report.md
    │   └── feature_request.md
    └── PULL_REQUEST_TEMPLATE.md
```
