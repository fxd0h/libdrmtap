# Security Model

## Overview

libdrmtap captures screen framebuffers by reading the Linux kernel's DRM/KMS
scanout. Reading the active scanout of a session you are not the DRM master of
requires `CAP_SYS_ADMIN`. This document describes, accurately, how the privilege
is isolated, what the helper actually does, and what it does **not** do.

This document is intended to match the code in `helper/drmtap-helper.c` and
`src/privilege_helper.c`. If you find a claim here that the code does not
support, that is a bug in this document — please report it.

---

## The two-process architecture

```
┌───────────────────────┐                    ┌────────────────────────┐
│  YOUR APPLICATION     │                    │   drmtap-helper        │
│  (unprivileged)       │◄── socketpair ────►│   (CAP_SYS_ADMIN)      │
│                       │                    │                        │
│  - Uses libdrmtap API │◄── DMA-BUF fd ─────│   - Opens /dev/dri/card │
│  - Processes frames   │                    │   - drmModeGetFB2       │
│  - Encodes / transports│                   │   - Exports DMA-BUF     │
└───────────────────────┘                    └────────────────────────┘
     unprivileged                            keeps CAP_SYS_ADMIN for its
     no network in the lib                   whole lifetime (see below)
```

The library (linked into your unprivileged application) `fork()`/`execl()`s the
helper, handing it one end of an anonymous `socketpair(2)` on fd 3
(`HELPER_SOCKET_FD`). The helper refuses to run if fd 3 is not a valid socket,
so it cannot be usefully invoked standalone. All capture requests go over that
private inherited socket.

---

## Why CAP_SYS_ADMIN

The privileged operation is `drmModeGetFB2()` to obtain the active scanout
framebuffer's GEM handles on a card node where the helper is **not** the DRM
master. The kernel gates that handle disclosure on
`drm_is_current_master() || CAP_SYS_ADMIN`. There is no narrower capability for
this specific operation:

- `CAP_DAC_OVERRIDE` or the `video` group only get you a device fd, not the
  handle-disclosure path.
- A render node has no KMS/scanout access at all.

So `CAP_SYS_ADMIN` is what the kernel requires. We are upfront that it is broad
and overpowered for what the helper actually does with it. The mitigation is
**structural** (keep it out of the main process, confine the helper), not a
clever capability minimization. The helper never becomes DRM master (it does not
call `drmSetMaster`).

> A narrower kernel capability for scanout read-back would let us drop
> `CAP_SYS_ADMIN` entirely. We would welcome such a change.

---

## What the helper actually does to confine itself

At startup, in this order (`main()` in `helper/drmtap-helper.c`):

1. **Refuses to run standalone** — validates that fd 3 is a socket; exits otherwise.
2. **Restricts the device path** — the device comes from `argv[1]` or the
   `DRM_DEVICE` env var (both attacker-influenceable). The helper canonicalizes
   it with `realpath()` and **refuses any path that does not resolve under
   `/dev/dri/`** before opening it.
3. **`PR_SET_NO_NEW_PRIVS`** — set via `prctl`, before seccomp; hard-fails if it
   cannot be set.
4. **Drops all capabilities except `CAP_SYS_ADMIN`** — via libcap
   (`cap_set_proc`). **Hard-fails** (refuses to serve) if it cannot.
5. **Installs a seccomp filter** — `seccomp_init(SCMP_ACT_KILL_PROCESS)` (a
   default-kill allowlist), allowing only: `read, write, close, openat, open,
   ioctl, sendto, sendmsg, recvfrom, mmap, munmap, brk, fstat, newfstatat,
   fcntl, exit_group, exit, rt_sigreturn, clock_gettime`. **Hard-fails** if it
   cannot install.
6. Opens the DRM device once and serves grab/cursor requests in a loop.

Both libcap and libseccomp are compiled in for the packaged (deb) build.

### Request protocol / attack surface

The wire protocol is tiny: three commands (`CMD_GRAB`, `CMD_GET_CURSOR`,
`CMD_QUIT`) in a fixed 8-byte struct. The only attacker-controllable per-request
field is a `u32 crtc_id`, used purely as an equality filter against a plane's
CRTC id — a bad value yields "no active framebuffer", not memory unsafety. The
request path carries no path or fd field. Fd passing (`SCM_RIGHTS`) is
one-directional, **helper → main only** (the DMA-BUF export); the unprivileged
side cannot hand the privileged side a path or fd to open.

---

## Honest caveats (what it does NOT do)

Do not read the above as more locked down than it is:

- **CAP_SYS_ADMIN is held for the helper's entire lifetime.** Every frame
  re-issues `drmModeGetFB2`, so the capability cannot be dropped after init. The
  helper reduces to exactly one capability and keeps it for the whole session.
  It is **not** dropped after initialization.
- **The helper is dynamically linked** (it links `libdrm`, `libcap`,
  `libseccomp`, and EGL/GLES). It is not statically linked. On a setcap binary
  the dynamic loader ignores `LD_PRELOAD`/`LD_LIBRARY_PATH` for the secure-exec
  case, but this is the loader's `AT_SECURE` behavior, not something the helper
  itself enforces.
- **The seccomp filter has no per-argument filtering.** `ioctl` is allowed on
  any fd; `open`/`openat` on any path. It blocks whole syscall classes, it does
  not narrow the allowed ones.
- **No `SO_PEERCRED` check.** Isolation rests on the private inherited
  socketpair — no third party can connect to it — not on a peer-credential check.
- The helper does **not** sanitize its environment (`clearenv`), set
  `PR_SET_DUMPABLE`, detect/refuse containers, log to syslog, set rlimits, or
  emit desktop notifications. (Earlier versions of this document claimed some of
  these; they were not in the code and have been removed.)

---

## Threat model

### Protected against

| Threat | Protection |
|---|---|
| Unprivileged app reads the screen | Kernel blocks `drmModeGetFB2` without DRM master / CAP_SYS_ADMIN |
| Unprivileged side points the helper at an arbitrary device | Device path must canonicalize under `/dev/dri/` |
| Unprivileged side makes the helper open an arbitrary path/fd | The request protocol carries no path/fd; fd passing is helper -> main only |
| Helper gains new privileges via exec | `PR_SET_NO_NEW_PRIVS` |
| Helper runs unconfined if hardening fails | Hard-fails: refuses to serve if cap-drop or seccomp cannot be established |
| Third party intercepts frames or connects to the helper | Anonymous inherited socketpair, not discoverable, no listening socket |
| Replacing the helper binary | setcap xattrs are cleared by the kernel on file modification; re-applying needs root |

### NOT protected against

| Threat | Why |
|---|---|
| Root reads the screen | Root can do anything; by design on Linux |
| Admin installs a malicious helper | If the admin/root is compromised, the system already is |
| Physical access | Physical access is full access |
| Insecure transport by the app | Transport security is the application's responsibility |
| A bug in the helper while it holds CAP_SYS_ADMIN | The helper is small and seccomp-confined, but a memory-safety bug while holding CAP_SYS_ADMIN is the residual risk; the cap is not dropped after init |

### Relationship to Wayland

This library deliberately reads below the Wayland compositor's per-application
isolation. That is intentional for use cases where administrator control
supersedes per-app isolation (servers with no logged-in user, headless
management, kiosks, login-screen capture, CI). For interactive desktops where
user consent matters, the portal/PipeWire path is the right tool — this is not a
replacement for it. Integrators on multi-user desktops should pair capture with
a user-visible indicator.

---

## Installation, audit, removal

```bash
# An administrator consciously grants the capability to the helper binary:
sudo setcap cap_sys_admin+ep /usr/lib/rustdesk/drmtap-helper   # (path varies by integration)

# Audit: find capability-bearing binaries
sudo getcap -r / 2>/dev/null | grep drmtap
getcap /usr/lib/rustdesk/drmtap-helper
ps aux | grep drmtap-helper

# Remove the capability, or the binary
sudo setcap -r /usr/lib/rustdesk/drmtap-helper
sudo rm /usr/lib/rustdesk/drmtap-helper
```

An unprivileged user cannot grant the capability; only root/an admin can.

---

## Reporting security issues

Please do not open a public issue for a vulnerability. Use GitHub's private
vulnerability reporting (Security -> Report a vulnerability) with a description,
reproduction, and impact.
