# Security Model

## Overview

libdrmtap captures screen framebuffers by accessing the Linux kernel's DRM/KMS subsystem. This requires elevated privileges — specifically `CAP_SYS_ADMIN` — to read framebuffer handles from the kernel.

This document explains the security architecture, threat model, known risks, and how libdrmtap compares to other approaches.

> **This document has been audited** from 10 expert perspectives (5 security experts + 5 red team hackers). See the audit findings that shaped this version in the project research docs.

---

## How Privileges Work

### The 2-process architecture

```
┌───────────────────────┐                    ┌───────────────────────┐
│  YOUR APPLICATION     │                    │   drmtap-helper       │
│  (unprivileged)       │◄── socketpair ────►│   (CAP_SYS_ADMIN)     │
│                       │                    │                       │
│  - Uses libdrmtap API │◄── DMA-BUF fd ─────│   - Opens /dev/dri/*  │
│  - Processes frames   │                    │   - Reads framebuffer │
│  - Encodes video      │                    │   - Passes fd back    │
│  - Network transport  │                    │   - Drops privileges  │
└───────────────────────┘                    └───────────────────────┘
     No privileges                           CAP_SYS_ADMIN (dropped after init)
     No network access                       No network access
     Full application logic                  ~500 lines of C, statically linked
```

### What CAP_SYS_ADMIN actually grants

> ⚠️ **Honest disclosure**: CAP_SYS_ADMIN is the most overloaded capability in the Linux kernel. It grants far more than DRM access.

`CAP_SYS_ADMIN` allows a process to:
- Call `drmModeGetFB2()` to read framebuffer handles ← **what we need**
- Mount/unmount filesystems ← we don't use this
- Use `ptrace` in some configurations ← we don't use this
- Perform various system administration tasks ← we don't use this
- Override resource limits ← we don't use this

**We need this capability for one purpose only**: obtaining framebuffer GEM handles from the DRM subsystem. Unfortunately, the Linux kernel does not provide a narrower capability for DRM framebuffer access. We mitigate this by **dropping the capability immediately** after obtaining the handle, and by restricting the helper with seccomp.

This is different from tools like `ping` (which uses the narrow `CAP_NET_RAW`). We acknowledge that `CAP_SYS_ADMIN` is overpowered for our use case and would welcome a future kernel change that introduces a more specific capability.

### Installation

```bash
# The administrator explicitly grants the capability:
sudo setcap cap_sys_admin+ep /usr/local/bin/drmtap-helper

# This is analogous to other privileged system tools.
# The administrator makes a conscious decision to grant this access.
```

The key point: **an administrator must consciously install and configure the helper**. It cannot be done by an unprivileged user.

---

## Threat Model

### What we protect against

| Threat | Protection |
|---|---|
| Unprivileged app reads screen | ✅ Kernel blocks `drmModeGetFB2` without CAP_SYS_ADMIN |
| Attacker replaces helper binary | ✅ Binary is in root-owned path; setcap xattrs are removed on file replacement |
| Other processes intercept frames | ✅ Communication uses anonymous socketpair, not discoverable |
| Helper binary has exploitable bug | ✅ Minimized: ~500 LOC, static linking, seccomp, privilege drop |
| Network attacker captures frames | ✅ Helper has zero network access; transport is the app's responsibility |
| LD_PRELOAD injection | ✅ Helper is statically linked and sanitizes environment on startup |
| Container escape via helper | ✅ Helper detects container environment and refuses to run |

### What we do NOT protect against

| Threat | Why |
|---|---|
| Root user reads screen | Root can do anything — this is by design on Linux |
| Admin installs malicious helper | If admin is compromised, the system is already compromised |
| Physical access to machine | Physical access = full access on any system |
| App transmits frames insecurely | Transport security is the application's responsibility, not libdrmtap's |

### Wayland security relationship

> **Honest statement**: This library deliberately operates below the Wayland compositor's application isolation layer. DRM/KMS capture reads framebuffer content that the compositor did not share with the capturing process.

This is **intentional** for use cases where system administrator control supersedes per-application isolation:
- Remote desktop for servers (no logged-in user to approve)
- Headless machine management
- Kiosks and digital signage
- Automated testing and CI

For desktop environments where user consent is desired, we recommend integrating with polkit to provide administrator-configurable policies.

---

## Attack Vector Analysis

### Vector 1: Exploiting the helper binary

**Risk**: LOW

The helper is hardened through multiple layers:
- **Anti-ptrace**: `prctl(PR_SET_DUMPABLE, 0)` as the very first action in `main()` — prevents ptrace attachment and core dumps
- **Static linking**: no LD_PRELOAD or LD_LIBRARY_PATH attacks
- **Environment sanitization**: all environment variables cleared on startup
- **Seccomp filtering**: restricts syscalls to `openat`, `close`, `read`, `write`, `ioctl`, `mmap`, `munmap`, `sendmsg`, `recvmsg`, `newfstatat`, `brk`, `exit_group`
- **Privilege drop**: CAP_SYS_ADMIN is dropped via `capset()` immediately after obtaining DRM handles
- **No new privileges**: `prctl(PR_SET_NO_NEW_PRIVS, 1)` prevents further escalation
- **No input parsing**: receives no user-controlled data
- **~500 LOC**: small enough to fully audit

**Initialization order** (security-critical sequence):
```
1. prctl(PR_SET_DUMPABLE, 0)     ← prevent ptrace
2. clearenv()                     ← sanitize environment
3. open /dev/dri/cardN            ← get DRM device
4. drmModeGetFB2() + PrimeToFD()  ← obtain handles (needs CAP_SYS_ADMIN)
5. capset() → drop CAP_SYS_ADMIN  ← privilege drop
6. prctl(PR_SET_NO_NEW_PRIVS, 1)  ← prevent re-escalation
7. seccomp install                ← lock down syscalls
8. enter main loop                ← serve fd requests
```

### Vector 2: Binary replacement

**Risk**: NEGLIGIBLE

- Binary lives in root-owned directory (`/usr/local/bin/`)
- `setcap` extended attributes are **automatically removed** by the kernel when the file is modified
- Attacker needs root to re-apply `setcap` — if they have root, they don't need the helper

### Vector 3: DMA-BUF fd interception

**Risk**: NEGLIGIBLE

- `socketpair()` creates an anonymous socket — no filesystem path, not in `/proc/net`
- `SCM_RIGHTS` transfers the fd only to the connected peer
- Parent process validated via `SO_PEERCRED` (UID/PID check)

### Vector 4: LD_PRELOAD / Environment injection

**Risk**: MITIGATED (previously HIGH)

- Helper is **statically linked** (no dynamic loader involved)
- Environment is sanitized: `clearenv()` on startup before any operations
- Kernel's `AT_SECURE` flag is respected

### Vector 5: Stalkerware / Unauthorized surveillance

**Risk**: SYSTEM ADMINISTRATION CONCERN

If someone with sudo access installs drmtap-helper for unauthorized surveillance:

**Detections available:**
```bash
# Find all binaries with capabilities
sudo getcap -r / 2>/dev/null | grep drmtap

# Check running helper processes
ps aux | grep drmtap-helper

# Audit log (if audit logging is enabled)
ausearch -c drmtap-helper
```

**Mitigations built into the helper:**
- Optional syslog logging on startup (enabled by default)
- Optional D-Bus desktop notification when capture starts
- Configurable maximum session duration (auto-exit after timeout)

### Vector 6: Container escape

**Risk**: MITIGATED

- Helper detects container environments via `/proc/1/cgroup` and namespace checks
- **Refuses to run inside containers with CAP_SYS_ADMIN** — this combination is dangerous
- Recommended pattern: run helper on the host, pass fd into container via mounted socket

### Vector 7: DRM kernel code path vulnerabilities

**Risk**: LOW (kernel-dependent)

The helper triggers kernel code paths (`drmModeGetFB2`, `drmPrimeHandleToFD`) that have had CVEs historically. Mitigations:
- All DRM ioctl return values validated
- Resource limits set via `setrlimit()`
- Recommend kernel ≥ 6.1 LTS for latest DRM security fixes

---

## Comparison with Other Approaches

| Solution | Runs as | Network | Code size | Static link | Seccomp | Auditable | Risk |
|---|---|---|---|---|---|---|---|
| **NoMachine** | root | ✅ Port 4000 | ~hundreds KLOC | ❌ | ❌ | ❌ Proprietary | 🔴 HIGH |
| **ssh (sshd)** | root | ✅ Port 22 | ~100 KLOC | ❌ | ✅ | ✅ OSS | 🟡 MEDIUM |
| **VNC + sudo** | root | ✅ Port 5900 | ~50 KLOC | ❌ | ❌ | ✅ OSS | 🟡 MEDIUM |
| **drmtap-helper** | CAP_SYS_ADMIN | ❌ | **~500 LOC** | ✅ | ✅ | ✅ OSS | 🟢 **LOW** |

### Why we're safer than NoMachine

NoMachine runs an entire server as root, listening on a network port, parsing complex protocols, managing sessions. Any vulnerability grants full remote root access.

libdrmtap's helper is local-only, network-free, statically linked, seccomp-filtered, and drops privileges after initialization. It does one thing: read a framebuffer handle and pass it to its parent.

---

## Helper Hardening Checklist

The helper binary implements these security measures:

- [x] `prctl(PR_SET_DUMPABLE, 0)` as first action (prevents ptrace and core dumps)
- [x] Static linking (no LD_PRELOAD attacks)
- [x] Environment sanitization (`clearenv()` on startup)
- [x] Seccomp syscall filter (allowlist of ~12 syscalls)
- [x] Capability drop after DRM init (`capset()` removes CAP_SYS_ADMIN)
- [x] `prctl(PR_SET_NO_NEW_PRIVS, 1)` — no further privilege escalation
- [x] Parent process validation via `SO_PEERCRED`
- [x] Container detection and refusal
- [x] Syslog logging of start/stop events
- [x] All DRM ioctl return values validated
- [x] Resource limits set via `setrlimit()`
- [x] No user input parsing
- [x] No network access
- [x] Open source and auditable (~500 LOC)

---

## Security Best Practices for Users

### Installation

```bash
# ✅ RECOMMENDED: Use setcap (minimal privilege)
sudo setcap cap_sys_admin+ep /usr/local/bin/drmtap-helper

# ⚠️ ACCEPTABLE: SUID root (helper must call setuid(getuid()) + setgid(getgid())
#    immediately after DRM init to drop back to the real user)
sudo chmod u+s /usr/local/bin/drmtap-helper

# ❌ NOT RECOMMENDED: Running your entire application as root
sudo ./my-application  # Don't do this

# ❌ NEVER: Run inside containers with CAP_SYS_ADMIN
docker run --cap-add=SYS_ADMIN ...  # Don't do this with drmtap
```

### Auditing

```bash
# List all binaries with capabilities on the system
sudo getcap -r / 2>/dev/null

# Check specifically for drmtap-helper
getcap /usr/local/bin/drmtap-helper

# Monitor helper execution via syslog
journalctl -f | grep drmtap

# Linux Audit subsystem
sudo auditctl -w /usr/local/bin/drmtap-helper -p x -k drmtap
ausearch -k drmtap
```

### Removal

```bash
# Remove capabilities
sudo setcap -r /usr/local/bin/drmtap-helper

# Or remove the binary entirely
sudo rm /usr/local/bin/drmtap-helper
```

---

## Supply Chain Security

- All releases are **signed with GPG** and include SHA256 checksums
- **Reproducible builds** — verify binary matches source with `meson compile -C build && sha256sum build/helper/drmtap-helper`
- Changes to `helper/drmtap-helper.c` require **2+ maintainer approval**
- CI uses GitHub artifact attestation and SLSA provenance
- Dependencies are pinned in `meson.build` — no wildcard versions

---

## Reporting Security Issues

If you discover a security vulnerability in libdrmtap:

1. **Do NOT open a public GitHub issue**
2. Use GitHub's private vulnerability reporting: **Security → Report a vulnerability**
3. Include: description, reproduction steps, and potential impact
4. We will respond within 48 hours and coordinate a fix before public disclosure
5. We follow responsible disclosure and will credit reporters (unless they prefer anonymity)

---

## Guidance for Desktop Integrators

On multi-user desktop systems where the logged-in user's consent matters, integrators should combine DRM capture with a **user-visible indicator** (tray icon, notification, or compositor protocol) to maintain user trust. The helper's optional D-Bus notification can serve as a starting point.

---

## The Philosophy

> This library deliberately operates below the Wayland compositor layer
> to capture framebuffer content via DRM/KMS. This is a system administration
> tool — controlled by Linux capabilities, authorized by administrators,
> and hardened through static linking, seccomp, and privilege dropping.
>
> Like all system administration tools, libdrmtap can be misused.
> We provide detection and logging mechanisms, but ultimately,
> controlling who has sudo access to your system is the primary
> defense against unauthorized use.
>
> We are transparent about what this tool does, how it does it,
> and what the risks are. Security is not about hiding capabilities —
> it's about making informed decisions.
