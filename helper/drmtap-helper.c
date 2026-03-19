/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file drmtap-helper.c
 * @brief Privileged helper binary — opens DRM with CAP_SYS_ADMIN and passes
 *        file descriptors to the unprivileged library via SCM_RIGHTS.
 *
 * Installation:
 *   sudo cp drmtap-helper /usr/local/bin/drmtap-helper
 *   sudo setcap cap_sys_admin+ep /usr/local/bin/drmtap-helper
 *
 * Protocol:
 *   - Helper inherits a Unix socket on fd 3 (HELPER_SOCKET_FD)
 *   - Receives requests: 1 byte command
 *   - Sends responses: 1 byte status + optional DMA-BUF fd via SCM_RIGHTS
 *
 * Commands:
 *   CMD_GRAB (0x01): Open DRM, get FB2, export as DMA-BUF, send fd
 *   CMD_QUIT (0xFF): Clean exit
 *
 * Security:
 *   - Validates socket fd on startup
 *   - Drops all caps except CAP_SYS_ADMIN (if libcap available)
 *   - Installs seccomp filter (if libseccomp available)
 *   - Sets FD_CLOEXEC on all opened fds
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/un.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#ifdef HAVE_LIBCAP
#include <sys/capability.h>
#endif

#ifdef HAVE_SECCOMP
#include <seccomp.h>
#endif

/* virtio-gpu transfer support — needed to pull pixels from host GPU to guest RAM */
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/virtio_gpu.h>
#ifndef DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST
struct drm_virtgpu_3d_transfer_from_host {
    __u32 bo_handle;
    __u32 pad;
    __u64 offset;
    __u32 level;
    __u32 stride;
    __u32 layer_stride;
    struct drm_virtgpu_3d_box {
        __u32 x, y, z, w, h, d;
    } box;
};
struct drm_virtgpu_3d_wait {
    __u32 handle;
    __u32 flags;
};
#define DRM_VIRTGPU_TRANSFER_FROM_HOST 0x07
#define DRM_VIRTGPU_WAIT              0x08
#define DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_TRANSFER_FROM_HOST, \
             struct drm_virtgpu_3d_transfer_from_host)
#define DRM_IOCTL_VIRTGPU_WAIT \
    DRM_IOW(DRM_COMMAND_BASE + DRM_VIRTGPU_WAIT, \
            struct drm_virtgpu_3d_wait)
#endif
#endif

/* Socket fd inherited from parent (via socketpair) */
#define HELPER_SOCKET_FD 3

/* Protocol commands */
#define CMD_GRAB 0x01
#define CMD_QUIT 0xFF

/* Response status */
#define RESP_OK    0x00
#define RESP_ERROR 0x01

/* Metadata sent before pixel data — must match helper_grab_result_t in library */
#define FLAG_HAS_DMABUF 0x01

struct grab_metadata {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t fb_id;
    uint32_t data_size;
    uint64_t modifier;
    uint32_t seq;
    uint64_t timestamp_ms;
    uint32_t flags;
    uint32_t _pad;
};

/* Send a file descriptor via SCM_RIGHTS */
static int send_fd(int sock, int fd, const void *meta, size_t meta_len) {
    struct msghdr msg = {0};
    struct iovec iov;
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } cmsg_buf;

    iov.iov_base = (void *)meta;
    iov.iov_len = meta_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    msg.msg_control = cmsg_buf.buf;
    msg.msg_controllen = sizeof(cmsg_buf.buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    ssize_t n = sendmsg(sock, &msg, MSG_NOSIGNAL);
    if (n < 0) {
        perror("sendmsg SCM_RIGHTS");
        return -1;
    }
    return 0;
}

/* ========================================================================= */
/* Security hardening                                                        */
/* ========================================================================= */

#ifdef HAVE_LIBCAP
// Drop all capabilities except CAP_SYS_ADMIN
static int drop_caps(void) {
    cap_t caps = cap_init();
    if (!caps) {
        perror("cap_set_proc failed"); return -1;
    }

    cap_value_t keep[] = { CAP_SYS_ADMIN };
    if (cap_set_flag(caps, CAP_PERMITTED, 1, keep, CAP_SET) != 0 ||
        cap_set_flag(caps, CAP_EFFECTIVE, 1, keep, CAP_SET) != 0) {
        cap_free(caps);
        perror("cap_set_proc failed"); return -1;
    }

    int ret = cap_set_proc(caps);
    cap_free(caps);

    if (ret == 0) {
        fprintf(stderr, "drmtap-helper: dropped caps, keeping CAP_SYS_ADMIN\n");
    }
    return ret;
}
#endif

#ifdef HAVE_SECCOMP
// Install seccomp filter allowing only needed syscalls
static int install_seccomp(void) {
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_KILL_PROCESS);
    if (!ctx) {
        perror("cap_set_proc failed"); return -1;
    }

    int allowed[] = {
        SCMP_SYS(read), SCMP_SYS(write), SCMP_SYS(close),
        SCMP_SYS(openat), SCMP_SYS(open),
        SCMP_SYS(ioctl),       /* DRM ioctls */
        SCMP_SYS(sendto),      /* send() */
        SCMP_SYS(sendmsg),     /* sendmsg() for SCM_RIGHTS */
        SCMP_SYS(recvfrom),    /* recv() */
        SCMP_SYS(mmap), SCMP_SYS(munmap),
        SCMP_SYS(brk),         /* malloc */
        SCMP_SYS(fstat), SCMP_SYS(newfstatat),
        SCMP_SYS(fcntl),
        SCMP_SYS(exit_group), SCMP_SYS(exit),
        SCMP_SYS(rt_sigreturn),
    };

    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, allowed[i], 0) != 0) {
            seccomp_release(ctx);
            perror("cap_set_proc failed"); return -1;
        }
    }

    int ret = seccomp_load(ctx);
    seccomp_release(ctx);

    if (ret == 0) {
        fprintf(stderr, "drmtap-helper: seccomp filter installed\n");
    }
    return ret;
}
#endif

/* ========================================================================= */
/* Socket helpers                                                            */
/* ========================================================================= */

// Send all bytes, handling partial writes
static int send_all(int sock, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            perror("cap_set_proc failed"); return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

// Send error: metadata with data_size=0
static int send_error(int sock, const char *reason) {
    fprintf(stderr, "drmtap-helper: %s\n", reason);
    struct grab_metadata meta = {0};  /* data_size=0 signals error */
    send_all(sock, &meta, sizeof(meta));
    perror("cap_set_proc failed"); return -1;
}

/* ========================================================================= */
/* DRM capture (privileged) — reads pixels, sends via socket                 */
/* ========================================================================= */

// Grab current framebuffer. Helper reads pixels in its own process
// (where dumb_mmap returns fresh data) and sends them via the socket.
static int grab_and_send(int sock, int drm_fd, int is_virtio) {
    void *mapped = MAP_FAILED;
    size_t mapped_size = 0;

    /* Refresh plane state on persistent fd */
    drmModePlaneRes *planes = drmModeGetPlaneResources(drm_fd);
    if (!planes) {
        return send_error(sock, "drmModeGetPlaneResources failed");
    }

    uint32_t fb_id = 0;
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *plane = drmModeGetPlane(drm_fd, planes->planes[i]);
        if (plane) {
            if (plane->fb_id != 0) {
                fb_id = plane->fb_id;
                drmModeFreePlane(plane);
                break;
            }
            drmModeFreePlane(plane);
        }
    }
    drmModeFreePlaneResources(planes);

    if (fb_id == 0) {
        return send_error(sock, "no active framebuffer found");
    }

    /* Use persistent DRM fd for ALL operations.
     * On virtio-gpu 3D (virgl), fresh fds don't maintain the GPU context
     * needed for transfer_from_host to actually refresh pixel data.
     * The persistent fd keeps the context alive — same as VNC direct path. */

    /* GetFB2 on persistent fd */
    drmModeFB2 *fb2 = drmModeGetFB2(drm_fd, fb_id);
    if (!fb2) {
        return send_error(sock, "drmModeGetFB2 failed");
    }

    if (fb2->handles[0] == 0) {
        drmModeFreeFB2(fb2);
        return send_error(sock, "handles[0]==0 (CAP_SYS_ADMIN not set?)");
    }

    uint32_t gem_handle = fb2->handles[0];

    /* virtio_gpu: transfer pixels from host GPU to guest RAM */
    if (is_virtio) {
#ifdef __linux__
        struct drm_virtgpu_3d_transfer_from_host xfer = {0};
        xfer.bo_handle = gem_handle;
        xfer.box.w = fb2->width;
        xfer.box.h = fb2->height;
        xfer.box.d = 1;
        if (drmIoctl(drm_fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &xfer) == 0) {
            struct drm_virtgpu_3d_wait wait_args = {0};
            wait_args.handle = gem_handle;
            drmIoctl(drm_fd, DRM_IOCTL_VIRTGPU_WAIT, &wait_args);
        }
#endif
    }

    /* dumb_mmap — map the framebuffer in THIS process */
    mapped_size = (size_t)fb2->pitches[0] * fb2->height;
    struct drm_mode_map_dumb map = {0};
    map.handle = gem_handle;
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        drmModeFreeFB2(fb2);
        return send_error(sock, "MODE_MAP_DUMB failed");
    }
    mapped = mmap(NULL, mapped_size, PROT_READ, MAP_SHARED,
                  drm_fd, map.offset);
    if (mapped == MAP_FAILED) {
        drmModeFreeFB2(fb2);
        return send_error(sock, "mmap failed");
    }

    /* Build metadata */
    struct grab_metadata meta = {0};
    meta.width = fb2->width;
    meta.height = fb2->height;
    meta.stride = fb2->pitches[0];
    meta.format = fb2->pixel_format;
    meta.fb_id = fb_id;
    meta.data_size = (uint32_t)mapped_size;
    meta.modifier = fb2->modifier;

    drmModeFreeFB2(fb2);

    /* Stamp frame with sequence number and timestamp */
    static uint32_t seq_counter = 0;
    seq_counter++;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    meta.seq = seq_counter;
    meta.timestamp_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    /* Debug: print pixel values to verify freshness */
    {
        static int frame_num = 0;
        frame_num++;
        uint32_t *pixels = (uint32_t *)mapped;
        uint32_t stride_px = meta.stride / 4;
        uint32_t p_top = pixels[960 < (meta.stride/4) ? 960 : 0];
        uint32_t p_clock = (94 * stride_px + 960 < mapped_size/4) ?
                           pixels[94 * stride_px + 960] : 0;
        uint32_t p_mid = (500 * stride_px + 960 < mapped_size/4) ?
                         pixels[500 * stride_px + 960] : 0;
        if (frame_num <= 5 || frame_num % 60 == 0) {
            fprintf(stderr,
                "drmtap-helper: frame=%d fb=%u mod=0x%lx top=%08x clock=%08x mid=%08x\n",
                frame_num, meta.fb_id, (unsigned long)meta.modifier,
                p_top, p_clock, p_mid);
        }
    }

    int ret;

    /* Non-linear modifier: export DMA-BUF fd via drmPrimeHandleToFD
     * and send it via SCM_RIGHTS — the parent can then EGL-import it
     * for GPU deswizzle. We still send the mmap'd pixels as fallback data. */
    if (meta.modifier != 0 /* DRM_FORMAT_MOD_LINEAR */ && !is_virtio) {
        int prime_fd = -1;
        int prime_ret = drmPrimeHandleToFD(drm_fd, gem_handle,
                                            DRM_CLOEXEC | DRM_RDWR, &prime_fd);
        if (prime_ret == 0 && prime_fd >= 0) {
            meta.flags = FLAG_HAS_DMABUF;
            meta.data_size = 0;  /* no pixel data follows */
            ret = send_fd(sock, prime_fd, &meta, sizeof(meta));
            close(prime_fd);
            munmap(mapped, mapped_size);
            return ret;
        }
        /* drmPrimeHandleToFD failed — fall through to pixel data path */
        fprintf(stderr,
            "drmtap-helper: drmPrimeHandleToFD failed (%d), falling back to pixels\n",
            prime_ret);
    }

    /* Linear modifier (or Prime failed): send pixel data directly */
    meta.flags = 0;
    ret = send_all(sock, &meta, sizeof(meta));
    if (ret == 0) {
        ret = send_all(sock, mapped, mapped_size);
    }

    /* Cleanup — munmap but DON'T close drm_fd (it's persistent). */
    munmap(mapped, mapped_size);

    return ret;
}

/* ========================================================================= */
/* Main event loop                                                           */
/* ========================================================================= */

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* Validate socket fd */
    if (fcntl(HELPER_SOCKET_FD, F_GETFD) < 0) {
        fprintf(stderr,
                "drmtap-helper: fd %d not valid — must be spawned by "
                "libdrmtap, not run directly\n", HELPER_SOCKET_FD);
        return 1;
    }

    int sock = HELPER_SOCKET_FD;

    /* Default device path — can be overridden via argv */
    const char *device = "/dev/dri/card0";
    if (argc > 1) {
        device = argv[1];
    }

    /* Also check DRM_DEVICE env var */
    const char *env_dev = getenv("DRM_DEVICE");
    if (env_dev) {
        device = env_dev;
    }

#ifdef HAVE_LIBCAP
    if (drop_caps() != 0) {
        fprintf(stderr, "drmtap-helper: warning: failed to drop caps\n");
    }
#endif

#ifdef HAVE_SECCOMP
    if (install_seccomp() != 0) {
        fprintf(stderr, "drmtap-helper: warning: failed to install seccomp\n");
    }
#endif

    /* ================================================================
     * Persistent DRM fd — opened ONCE, reused for all frames.
     * This is the key performance optimization: avoids the expensive
     * open()/close() + drmSetClientCap() on every frame.
     * ================================================================ */
    int drm_fd = open(device, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        fprintf(stderr, "drmtap-helper: failed to open %s: %s\n",
                device, strerror(errno));
        return 1;
    }
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    /* Detect virtio_gpu once */
    int is_virtio = 0;
    drmVersion *ver = drmGetVersion(drm_fd);
    if (ver) {
        if (ver->name_len >= 9 &&
            strncmp(ver->name, "virtio_gpu", ver->name_len) == 0) {
            is_virtio = 1;
        }
        drmFreeVersion(ver);
    }
    fprintf(stderr, "drmtap-helper: persistent fd=%d device=%s virtio=%d\n",
            drm_fd, device, is_virtio);

    /* Event loop: receive commands, process with persistent fd */
    while (1) {
        uint8_t cmd;
        ssize_t n = recv(sock, &cmd, 1, 0);

        if (n <= 0) {
            break;
        }

        switch (cmd) {
            case CMD_GRAB:
                grab_and_send(sock, drm_fd, is_virtio);
                break;

            case CMD_QUIT:
                goto done;

            default:
                send_error(sock, "unknown command");
                break;
        }
    }

done:
    close(sock);
    return 0;
}
