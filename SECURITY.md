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
(`HELPER_SOCKET_FD`). The helper refuses to run if fd 3 is not a connected
socket, **and** checks the peer's credentials via `SO_PEERCRED`, refusing to
serve unless the peer uid matches its own — so it cannot be usefully invoked
standalone or hijacked by another user. All capture requests go over that
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

1. **Refuses to run standalone** — validates that fd 3 is a connected socket and
   that the peer uid (via `SO_PEERCRED`) matches its own uid; exits otherwise.
2. **Restricts the device path** — the device comes from `argv[1]` or the
   `DRM_DEVICE` env var (both attacker-influenceable). The helper canonicalizes
   it with `realpath()` and **refuses any path that does not resolve under
   `/dev/dri/`** before opening it.
3. **`PR_SET_NO_NEW_PRIVS`** — set via `prctl`, before seccomp; hard-fails if it
   cannot be set.
4. **Opens the DRM device once, read-only** — `open(..., O_RDONLY | O_CLOEXEC)`.
   Only read ioctls are issued (`GetFB2` / `PrimeHandleToFD` / `SetClientCap`),
   which all work on an `O_RDONLY` fd on the CAP_SYS_ADMIN path; the helper never
   becomes DRM master and never modifies KMS state. Opening happens **before**
   seccomp so the filter can forbid `open`/`openat` outright (next step).
5. **Drops all capabilities except `CAP_SYS_ADMIN`** — via libcap
   (`cap_set_proc`). **Hard-fails** (refuses to serve) if it cannot.
6. **Installs a seccomp filter** — `seccomp_init(SCMP_ACT_KILL_PROCESS)` (a
   default-**KILL** allowlist), allowing only: `read, write, close, ioctl,
   sendto, sendmsg, recvfrom, mmap, munmap, brk, fstat, newfstatat, fcntl,
   exit_group, exit, rt_sigreturn, clock_gettime`. **`open`/`openat` are
   deliberately NOT on the allowlist** — the device fd was already opened in
   step 4 and the grab loop only ever reuses it, so a compromised helper cannot
   open arbitrary files even while it holds `CAP_SYS_ADMIN`. **Hard-fails** if it
   cannot install.
7. Serves grab/cursor requests on the persistent fd in a loop.

The helper binary is also built with exploit-mitigation flags
(`-fstack-protector-strong`, `_FORTIFY_SOURCE=2` on optimized builds, PIE, and
full RELRO via `-z relro -z now`). libcap and libseccomp are compiled in for the
packaged build; if either confinement step fails to take effect the helper
refuses to serve rather than run unconfined. Failures along this path emit
accurate, per-condition diagnostics to stderr (which hardening step failed and
why).

### Request protocol / attack surface

The wire protocol is tiny: three commands (`CMD_GRAB`, `CMD_GET_CURSOR`,
`CMD_QUIT`) in a fixed 8-byte struct. The only attacker-controllable per-request
field is a `u32 crtc_id`, used purely as an equality filter against a plane's
CRTC id — a bad value yields "no active framebuffer", not memory unsafety. The
request path carries no path or fd field. Fd passing (`SCM_RIGHTS`) is
one-directional, **helper → main only** (the DMA-BUF export); the unprivileged
side cannot hand the privileged side a path or fd to open.

Before any size computation, the helper validates the framebuffer geometry
(`pitch`/`height`) reported by the kernel: it rejects a zero pitch/height and
caps `pitch * height` at one 8K BGRA frame (~126 MB). This guards against an
integer-overflow of `size_t` and keeps the `u32 data_size` put on the wire from
disagreeing with the payload actually sent, so a malformed scanout cannot drive
an oversized allocation or a short write.

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
  any fd. It blocks whole syscall classes (including `open`/`openat` entirely),
  but it does not narrow the argument values of the syscalls it does allow.
- **A world-executable setcap helper on a multi-user host is a real
  consideration.** Anyone who can execute the binary gets a process holding
  `CAP_SYS_ADMIN` (confined by seccomp, and the `SO_PEERCRED` check stops them
  driving *your* session's socket, but it is still a privileged process they can
  spawn). The recommended packaging is therefore to install it owned
  `root:<group>` mode `0750`, so only members of that group can execute it.
  libdrmtap ships the hardened binary; this world-exec deployment decision is the
  integrator's (e.g. RustDesk's) to make, not something libdrmtap enforces.
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
| Helper exec'd standalone or by a different user | Refuses unless fd 3 is a connected socket whose `SO_PEERCRED` peer uid matches its own |
| Unprivileged side points the helper at an arbitrary device | Device path must canonicalize under `/dev/dri/`, and the node is opened `O_RDONLY` |
| Unprivileged side makes the helper open an arbitrary path/fd | The request protocol carries no path/fd; fd passing is helper -> main only; `open`/`openat` are not on the seccomp allowlist after init |
| Malformed scanout geometry drives an oversized allocation / overflow | Geometry guard rejects zero/absurd `pitch`/`height`, capping at one 8K BGRA frame |
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
# Restrict who can execute the helper FIRST, so it is never world-executable
# while it already carries the capability (path varies by integration):
sudo chown root:rustdesk-capture /usr/lib/rustdesk/drmtap-helper
sudo chmod 0750 /usr/lib/rustdesk/drmtap-helper

# ...then an administrator consciously grants the capability to the binary:
sudo setcap cap_sys_admin+ep /usr/lib/rustdesk/drmtap-helper

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
