/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file privilege_helper.c
 * @brief Auto-spawn privileged helper and SCM_RIGHTS fd passing
 *
 * When the library detects that handles[0] == 0 (missing CAP_SYS_ADMIN),
 * it automatically spawns drmtap-helper via socketpair + fork/exec.
 * The helper opens the DRM device with CAP_SYS_ADMIN and passes back
 * DMA-BUF file descriptors via SCM_RIGHTS.
 *
 * Protocol:
 *   - Library creates socketpair(AF_UNIX, SOCK_STREAM)
 *   - fork/exec the helper with the child socket on fd 3
 *   - Send CMD_GRAB (0x01) to request a DMA-BUF fd
 *   - Receive status byte + optional fd via SCM_RIGHTS recvmsg
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "drmtap_internal.h"

/* Must match values in drmtap-helper.c */
#define HELPER_SOCKET_FD 3
#define CMD_GRAB 0x01
#define CMD_QUIT 0xFF
#define RESP_OK    0x00

/* ========================================================================= */
/* Helper search paths                                                       */
/* ========================================================================= */

static const char *helper_search_paths[] = {
    NULL,  /* slot 0: ctx->helper_path (if set) */
    "/usr/libexec/drmtap-helper",
    "/usr/local/libexec/drmtap-helper",
    "/usr/local/bin/drmtap-helper",
    "/usr/bin/drmtap-helper",
    "/usr/lib/drmtap/drmtap-helper",
    NULL
};

// Find the helper binary
static const char *find_helper(drmtap_ctx *ctx) {
    /* Check configured path first */
    if (ctx->helper_path[0]) {
        if (access(ctx->helper_path, X_OK) == 0) {
            return ctx->helper_path;
        }
        drmtap_debug_log(ctx, "configured helper not found: %s",
                         ctx->helper_path);
    }

    /* Search standard paths */
    for (int i = 1; helper_search_paths[i]; i++) {
        if (access(helper_search_paths[i], X_OK) == 0) {
            drmtap_debug_log(ctx, "found helper: %s", helper_search_paths[i]);
            return helper_search_paths[i];
        }
    }

    return NULL;
}

/* ========================================================================= */
/* Helper lifecycle                                                          */
/* ========================================================================= */

// Spawn the helper binary via socketpair + fork/exec
// Sets ctx->helper_fd and ctx->helper_pid on success
int drmtap_helper_spawn(drmtap_ctx *ctx) {
    if (ctx->helper_fd >= 0) {
        /* Already running */
        return 0;
    }

    const char *helper_path = find_helper(ctx);
    if (!helper_path) {
        drmtap_set_error(ctx,
            "drmtap-helper not found. Install it with:\n"
            "  sudo cp drmtap-helper /usr/libexec/drmtap-helper\n"
            "  sudo setcap cap_sys_admin+ep /usr/libexec/drmtap-helper");
        return -EACCES;
    }

    /* Create socket pair for IPC */
    int socks[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, socks) < 0) {
        drmtap_set_error(ctx, "socketpair failed: %s", strerror(errno));
        return -errno;
    }

    pid_t pid = fork();
    if (pid < 0) {
        drmtap_set_error(ctx, "fork failed: %s", strerror(errno));
        close(socks[0]);
        close(socks[1]);
        return -errno;
    }

    if (pid == 0) {
        /* ---- Child process ---- */
        close(socks[0]);  /* Close parent's end */

        /* Move child socket to fd HELPER_SOCKET_FD */
        if (socks[1] != HELPER_SOCKET_FD) {
            if (dup2(socks[1], HELPER_SOCKET_FD) < 0) {
                _exit(127);
            }
            close(socks[1]);
        }
        /* Clear CLOEXEC on the helper socket so it survives exec */
        fcntl(HELPER_SOCKET_FD, F_SETFD, 0);

        /* Pass device path as argv[1] */
        execl(helper_path, "drmtap-helper", ctx->device_path, NULL);

        /* exec failed */
        _exit(127);
    }

    /* ---- Parent process ---- */
    close(socks[1]);  /* Close child's end */

    ctx->helper_fd = socks[0];
    ctx->helper_pid = pid;

    drmtap_debug_log(ctx, "spawned helper pid=%d from %s", pid, helper_path);
    return 0;
}

// Kill the helper process
void drmtap_helper_stop(drmtap_ctx *ctx) {
    if (ctx->helper_fd >= 0) {
        /* Send quit command (best effort) */
        uint8_t cmd = CMD_QUIT;
        ssize_t n = send(ctx->helper_fd, &cmd, 1, MSG_NOSIGNAL);
        (void)n;

        close(ctx->helper_fd);
        ctx->helper_fd = -1;
    }

    if (ctx->helper_pid > 0) {
        /* Give helper 100ms to exit, then SIGKILL */
        int status;
        usleep(100000);
        if (waitpid(ctx->helper_pid, &status, WNOHANG) == 0) {
            kill(ctx->helper_pid, SIGKILL);
            waitpid(ctx->helper_pid, &status, 0);
        }
        ctx->helper_pid = -1;
    }
}

/* ========================================================================= */
/* SCM_RIGHTS fd receiving                                                   */
/* ========================================================================= */

/* recv_fd removed — V2 protocol sends pixel data directly via socket.
 *
 * NOTE: This means CCS-compressed framebuffers (Intel Gen12+) cannot be
 * deswizzled by the parent, since EGL requires a DMA-BUF fd. A future
 * V3 protocol should re-add SCM_RIGHTS fd passing for non-linear modifiers
 * so the parent can use EGL GPU deswizzle. See gpu_auto_process() in
 * drm_grab.c for the fallback behavior when dma_buf_fd == -1. */

/* ========================================================================= */
/* Public helper API (called from drm_grab.c)                                */
/* ========================================================================= */

// Receive exactly len bytes from socket, handling partial reads
static int recv_all(int sock, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t received = 0;
    while (received < len) {
        ssize_t n = recv(sock, p + received, len - received, 0);
        if (n <= 0) {
            return -1;
        }
        received += (size_t)n;
    }
    return 0;
}

// Request metadata + pixel data from the helper.
// The helper reads the framebuffer in its own process (fresh data)
// and sends metadata followed by raw pixel bytes through the socket.
// Returns 0 on success, negative errno on error.
int drmtap_helper_grab(drmtap_ctx *ctx, helper_grab_result_t *result,
                        void *pixel_buf, size_t buf_size) {
    if (ctx->helper_fd < 0) {
        int ret = drmtap_helper_spawn(ctx);
        if (ret < 0) {
            return ret;
        }
    }

    /* Send grab command */
    uint8_t cmd = CMD_GRAB;
    ssize_t n = send(ctx->helper_fd, &cmd, 1, MSG_NOSIGNAL);
    if (n != 1) {
        drmtap_debug_log(ctx, "helper send failed, trying respawn");
        drmtap_helper_stop(ctx);

        int ret = drmtap_helper_spawn(ctx);
        if (ret < 0) {
            return ret;
        }

        n = send(ctx->helper_fd, &cmd, 1, MSG_NOSIGNAL);
        if (n != 1) {
            drmtap_set_error(ctx, "helper communication failed after respawn");
            return -EIO;
        }
    }

    /* Receive metadata */
    memset(result, 0, sizeof(*result));
    if (recv_all(ctx->helper_fd, result, sizeof(*result)) < 0) {
        drmtap_set_error(ctx, "helper metadata recv failed");
        return -EIO;
    }

    /* data_size == 0 means error */
    if (result->data_size == 0) {
        drmtap_set_error(ctx, "helper returned error");
        return -EIO;
    }

    drmtap_debug_log(ctx, "helper: %ux%u fb=%u data_size=%u seq=%u ts=%llu",
                     result->width, result->height,
                     result->fb_id, result->data_size,
                     result->seq, (unsigned long long)result->timestamp_ms);

    /* Receive pixel data directly into caller's buffer */
    if (result->data_size > buf_size) {
        drmtap_set_error(ctx, "helper data_size %u exceeds buffer %zu",
                         result->data_size, buf_size);
        /* Drain the socket to stay in sync */
        size_t remaining = result->data_size;
        char drain[4096];
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(drain) ? remaining : sizeof(drain);
            recv(ctx->helper_fd, drain, chunk, 0);
            remaining -= chunk;
        }
        return -ENOSPC;
    }

    if (recv_all(ctx->helper_fd, pixel_buf, result->data_size) < 0) {
        drmtap_set_error(ctx, "helper pixel recv failed");
        return -EIO;
    }

    return 0;
}
