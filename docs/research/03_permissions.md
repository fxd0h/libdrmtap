# Permissions and DRM Security Model

> **Date**: 2026-03-14  
> **Sources**: kernel.org, gpu-screen-recorder, kmsvnc, FFmpeg, web research

---

## The Permissions Problem

DRM/KMS capture requires privileged access. There are several permission layers:

### Layer 1: Device access `/dev/dri/cardN`

```
crw-rw----+ 1 root video 226, 0 /dev/dri/card0
```

- **Requires**: user in `video` group or udev rules
- **udev rule**: `KERNEL=="card[0-9]*", SUBSYSTEM=="drm", GROUP="video", MODE="0660"`

### Layer 2: drmModeGetFB2() → framebuffer handles

Starting with Linux ~5.x, obtaining framebuffer handles requires **CAP_SYS_ADMIN**.

```c
fb2 = drmModeGetFB2(drm_fd, plane->fb_id);
// fb2->handles[0] will be 0 without CAP_SYS_ADMIN!
```

FFmpeg documents this explicitly:
```c
if (!fb2->handles[0]) {
    av_log(avctx, AV_LOG_ERROR, "No handle set on framebuffer: "
           "maybe you need some additional capabilities?\n");
}
```

### Layer 3: DRM Master

- Only one process can be "DRM master" at a time (normally the compositor)
- Some operations (like `DRM_IOCTL_GEM_FLINK`) require being master
- kmsvnc: calls `drmDropMaster()` when it doesn't need to be master
- A non-master process can use `drmPrimeHandleToFD` if it has CAP_SYS_ADMIN

### Layer 4: DMA-BUF Sync

```c
struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
ioctl(prime_fd, DMA_BUF_IOCTL_SYNC, &sync);
// read data
sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
ioctl(prime_fd, DMA_BUF_IOCTL_SYNC, &sync);
```

---

## Helper Binary Pattern (Industry Standard)

### How gpu-screen-recorder does it

```
┌─────────────────────┐         Unix         ┌───────────────────────┐
│  gpu-screen-recorder │ ◄──── Domain ────► │   gsr-kms-server      │
│  (unprivileged)      │       Socket        │  (cap_sys_admin+ep)   │
│                      │                     │                       │
│  - GPU encoding      │  ◄── DMA-BUF fd ── │  - open(/dev/dri/card)│
│  - User interface    │                     │  - drmModeGetFB2()    │
│  - File writing      │                     │  - drmPrimeHandleToFD │
└─────────────────────┘                     └───────────────────────┘
```

1. **gsr-kms-server** is a separate, small binary (~500 lines)
2. Granted `CAP_SYS_ADMIN` via `setcap`: `sudo setcap cap_sys_admin+ep gsr-kms-server`
3. Communicates with main app via **Unix domain socket**
4. Passes **DMA-BUF file descriptors** to main process via `SCM_RIGHTS` (sendmsg/recvmsg)
5. Main app needs **no special privileges**

### Advantages of this pattern
- Main app runs as normal user
- Attack surface of the privileged binary is minimal (~500 lines)
- DMA-BUF fd can be shared between processes via Unix sockets
- Compatible with Flatpak/AppImage (helper installed separately)

### Typical installation
```bash
# Option 1: setcap (recommended); install root:<group> mode 0750
sudo install -o root -g <group> -m 0750 drmtap-helper /usr/local/bin/drmtap-helper
sudo setcap cap_sys_admin+ep /usr/local/bin/drmtap-helper

# Option 2: SUID root (less secure)
sudo chmod u+s /usr/local/bin/drmtap-helper

# Option 3: polkit rule (enterprise)
# /usr/share/polkit-1/rules.d/50-drmtap.rules
```

---

## Permission Alternatives

| Method | Security | Complexity | Portability |
|---|---|---|---|
| Direct root | ❌ Very insecure | Trivial | ✅ Universal |
| setcap CAP_SYS_ADMIN | ✅ Reasonable | Low | ✅ |
| SUID binary | ⚠️ Acceptable | Low | ✅ |
| Polkit | ✅✅ Best | High | ❌ SystemD only |
| udev + group | ✅ For device | Low | ✅ Device access only |
| Kernel patch | ❌ Impractical | Very high | ❌ |

---

## Design Decision for libdrmtap

### 2-process architecture (like gpu-screen-recorder)

```c
// API that abstracts permission complexity:

// If app has CAP_SYS_ADMIN → direct capture
// If not → auto-spawn helper
DrmCapture *drmcapture_open(DrmCaptureConfig *config);

// Internally:
// 1. Try drmModeGetFB2() directly
// 2. If handles[0] == 0 → launch drmtap-helper
// 3. Helper passes DMA-BUF fds via Unix socket
// 4. User API is unaware of the difference
```

### Helper binary: drmtap-helper

Small privileged binary that:
1. Verifies it was spawned by the library — it inherits the socketpair on fd 3 and checks the peer uid via `SO_PEERCRED`, refusing any other caller.
2. Opens the DRM node `O_RDONLY`, restricted to a `realpath()` under `/dev/dri/` so symlinks/`..` cannot escape.
3. Sets `PR_SET_NO_NEW_PRIVS`, drops every capability except `CAP_SYS_ADMIN` (libcap), then installs a default-KILL seccomp allowlist (libseccomp). `open`/`openat` are deliberately **not** on the allowlist — the device is opened once *before* the filter loads, so a compromised helper cannot open arbitrary files even while holding `CAP_SYS_ADMIN`.
4. Enumerates planes/CRTCs and calls `drmModeGetFB2` + `drmPrimeHandleToFD`.
5. Passes the DMA-BUF fd to the parent via `SCM_RIGHTS` (V3 zero-copy); falls back to dumb-mapping and copying pixels over the socket (V2) when DMA-BUF export is not possible.

Built with exploit-mitigation flags (stack-protector-strong, FORTIFY, PIE, full RELRO); the build hard-fails if libcap or libseccomp is missing. **Recommended packaging:** install `root:<group>` mode `0750` — a world-executable file-capability binary on a multi-user host is a real consideration, and is handled on the consumer (RustDesk) side rather than inside libdrmtap.
