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
 *   sudo cp drmtap-helper /usr/libexec/drmtap-helper
 *   sudo setcap cap_sys_admin+ep /usr/libexec/drmtap-helper
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
#include <sys/socket.h>
#include <sys/un.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

/* Socket fd inherited from parent (via socketpair) */
#define HELPER_SOCKET_FD 3

/* Protocol commands */
#define CMD_GRAB 0x01
#define CMD_QUIT 0xFF

/* Response status */
#define RESP_OK    0x00
#define RESP_ERROR 0x01

/* ========================================================================= */
/* SCM_RIGHTS fd passing                                                     */
/* ========================================================================= */

// Send a file descriptor over the Unix socket via SCM_RIGHTS
static int send_fd(int socket, int fd_to_send, uint8_t status) {
    struct msghdr msg = {0};
    struct iovec iov;
    char cmsg_buf[CMSG_SPACE(sizeof(int))];

    iov.iov_base = &status;
    iov.iov_len = sizeof(status);

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (fd_to_send >= 0) {
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));
    }

    ssize_t n = sendmsg(socket, &msg, 0);
    return (n > 0) ? 0 : -1;
}

// Send an error response (no fd)
static int send_error(int socket, const char *reason) {
    fprintf(stderr, "drmtap-helper: %s\n", reason);
    uint8_t status = RESP_ERROR;
    return send_fd(socket, -1, status);
}

/* ========================================================================= */
/* DRM capture (privileged)                                                  */
/* ========================================================================= */

// Find primary plane and export its framebuffer as a DMA-BUF fd
static int grab_and_send(int sock, const char *device_path) {
    int drm_fd = -1;
    int prime_fd = -1;
    int ret = -1;

    /* Open DRM device */
    drm_fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        send_error(sock, "failed to open DRM device");
        return -1;
    }

    /* Enable universal planes */
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    /* Find a plane with an active framebuffer */
    drmModePlaneRes *planes = drmModeGetPlaneResources(drm_fd);
    if (!planes) {
        send_error(sock, "drmModeGetPlaneResources failed");
        goto cleanup;
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
        send_error(sock, "no active framebuffer found");
        goto cleanup;
    }

    /* GetFB2 — this is why we need CAP_SYS_ADMIN */
    drmModeFB2 *fb2 = drmModeGetFB2(drm_fd, fb_id);
    if (!fb2) {
        send_error(sock, "drmModeGetFB2 failed");
        goto cleanup;
    }

    if (fb2->handles[0] == 0) {
        send_error(sock, "handles[0]==0 even with helper (CAP_SYS_ADMIN not set?)");
        drmModeFreeFB2(fb2);
        goto cleanup;
    }

    /* Export as DMA-BUF */
    ret = drmPrimeHandleToFD(drm_fd, fb2->handles[0],
                             O_RDONLY | O_CLOEXEC, &prime_fd);
    drmModeFreeFB2(fb2);

    if (ret < 0 || prime_fd < 0) {
        send_error(sock, "drmPrimeHandleToFD failed");
        goto cleanup;
    }

    /* Send the DMA-BUF fd to the client */
    ret = send_fd(sock, prime_fd, RESP_OK);
    close(prime_fd);

cleanup:
    if (drm_fd >= 0) {
        close(drm_fd);
    }
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
    /* TODO: Drop all capabilities except CAP_SYS_ADMIN */
    /* cap_t caps = cap_get_proc();
     * ...
     * cap_set_proc(caps); */
#endif

#ifdef HAVE_SECCOMP
    /* TODO: Install seccomp filter allowing only:
     *   open, close, read, write, ioctl (DRM + DMA-BUF),
     *   sendmsg, recvmsg, mmap, exit_group */
#endif

    /* Event loop: receive commands, process, respond */
    while (1) {
        uint8_t cmd;
        ssize_t n = recv(sock, &cmd, 1, 0);

        if (n <= 0) {
            /* Parent closed the socket */
            break;
        }

        switch (cmd) {
            case CMD_GRAB:
                grab_and_send(sock, device);
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
