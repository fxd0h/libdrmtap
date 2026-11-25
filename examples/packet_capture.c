/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file packet_capture.c
 * @brief Split capture of a multi-plane scanout, end to end, in two processes.
 *
 * The EXPORTER (the parent here; a privileged service in production) captures
 * every lit plane of the CRTC with drmtap_grab_packet() and sends the packet over
 * a unix socket: the bytes as the message, the DMA-BUF fds as SCM_RIGHTS. It does
 * not look inside the packet. The CONVERTER (the child; an unprivileged process
 * in production) receives both and calls drmtap_convert_packet(), which composes
 * the planes into one CRTC-sized XRGB8888 frame on its render node.
 *
 * Compared with examples/split_capture.c (drmtap_grab_desc + one fd), this also
 * captures a window the compositor put on its own plane (an overlay, or an
 * underlay below a desktop with a hole, rustdesk/rustdesk#17187), and the caller
 * code does not change when what a frame contains changes.
 *
 * SOCK_SEQPACKET keeps one packet per message, so no framing is needed.
 *
 *   build/packet_capture            # one frame, auto-detected device
 *   build/packet_capture 100        # 100 frames, per-frame timing
 *   DRM_DEVICE=/dev/dri/card1 build/packet_capture
 *
 * Exit 0 on success or SKIP (no capture device / no render node), 1 on failure.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "drmtap.h"

/* What the converter reports for each frame. */
typedef struct {
    int      ok;          /* 1 converted, 0 failed, -1 no render node */
    int      err;         /* drmtap_convert_packet() return value */
    uint32_t width, height, stride, format;
    uint64_t nonblack;    /* pixels with any non-zero RGB */
    uint64_t checksum;
    double   convert_us;
} conv_result_t;

static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

/* One message: `len` bytes, plus `nfds` fds as SCM_RIGHTS when nfds > 0. */
static int send_msg(int sock, const void *data, size_t len, const int *fds, size_t nfds) {
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int) * DRMTAP_PACKET_MAX_FDS)];
    } cbuf;
    struct iovec iov = { .iov_base = (void *)data, .iov_len = len };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    memset(&cbuf, 0, sizeof(cbuf));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (nfds > 0) {
        msg.msg_control = cbuf.buf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
        memcpy(CMSG_DATA(cm), fds, sizeof(int) * nfds);
    }
    return sendmsg(sock, &msg, MSG_NOSIGNAL) < 0 ? -1 : 0;
}

/* One message into data (at most cap bytes) and up to DRMTAP_PACKET_MAX_FDS fds.
 * Returns the byte count, 0 at end of stream, -1 on error or truncation (every
 * received fd is closed then). */
static ssize_t recv_msg(int sock, void *data, size_t cap, int *fds, size_t *nfds) {
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int) * DRMTAP_PACKET_MAX_FDS)];
    } cbuf;
    struct iovec iov = { .iov_base = data, .iov_len = cap };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf.buf;
    msg.msg_controllen = sizeof(cbuf.buf);
    *nfds = 0;
    ssize_t n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); n >= 0 && cm; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            size_t k = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < k; i++) {
                int fd;
                memcpy(&fd, CMSG_DATA(cm) + i * sizeof(int), sizeof(int));
                if (*nfds < DRMTAP_PACKET_MAX_FDS) {
                    fds[(*nfds)++] = fd;
                } else {
                    close(fd);
                }
            }
        }
    }
    if (n > 0 && (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
        for (size_t i = 0; i < *nfds; i++) {
            close(fds[i]);
        }
        *nfds = 0;
        return -1;
    }
    return n;
}

/* ── CONVERTER (unprivileged child) ── */
static int run_converter(int sock) {
    drmtap_ctx *ctx = drmtap_open_render(NULL);
    if (!ctx) {
        conv_result_t r = { .ok = -1 };
        send_msg(sock, &r, sizeof(r), NULL, 0);
        return 0;
    }
    uint8_t bytes[DRMTAP_PACKET_MAX_BYTES];
    int fds[DRMTAP_PACKET_MAX_FDS];
    for (;;) {
        size_t nfds = 0;
        ssize_t n = recv_msg(sock, bytes, sizeof(bytes), fds, &nfds);
        if (n <= 0) {
            break;   /* exporter done */
        }
        conv_result_t r;
        memset(&r, 0, sizeof(r));
        drmtap_frame_info frame;
        double t0 = now_us();
        r.err = drmtap_convert_packet(ctx, bytes, (size_t)n, fds, nfds, &frame);
        r.convert_us = now_us() - t0;
        /* The fds are ours to close; the GPU import holds its own reference. */
        for (size_t i = 0; i < nfds; i++) {
            close(fds[i]);
        }
        if (r.err == 0 && frame.data) {
            r.ok = 1;
            r.width = frame.width;
            r.height = frame.height;
            r.stride = frame.stride;
            r.format = frame.format;
            for (uint32_t y = 0; y < frame.height; y++) {
                const uint8_t *row = (const uint8_t *)frame.data + (size_t)y * frame.stride;
                for (uint32_t x = 0; x < frame.width; x++) {
                    const uint8_t *px = row + (size_t)x * 4;
                    if (px[0] | px[1] | px[2]) {
                        r.nonblack++;
                    }
                    r.checksum += (uint64_t)px[0] + px[1] * 3u + px[2] * 7u;
                }
            }
        } else {
            fprintf(stderr, "converter: drmtap_convert_packet failed (%d): %s\n", r.err,
                    drmtap_error(ctx) ? drmtap_error(ctx) : "");
        }
        if (send_msg(sock, &r, sizeof(r), NULL, 0) != 0) {
            break;
        }
    }
    drmtap_close(ctx);
    return 0;
}

/* ── EXPORTER (parent) ── */
static int run_exporter(int sock, int frames) {
    drmtap_ctx *ctx = drmtap_open(NULL);
    if (!ctx) {
        printf("  SKIP: no capture device (%s)\n",
               drmtap_error(NULL) ? drmtap_error(NULL) : "drmtap_open failed");
        return 0;
    }
    int failures = 0;
    double grab_total = 0, conv_total = 0;
    int converted = 0;
    for (int i = 0; i < frames; i++) {
        drmtap_packet pkt;
        double t0 = now_us();
        int r = drmtap_grab_packet(ctx, &pkt);
        for (int tries = 0; r == -EAGAIN && tries < 100; tries++) {
            /* "no frame this time" (a plane flipped during the export, an old helper
             * in an underlay): keep trying for up to ~1 s, it is not a failure */
            drmtap_packet_release(&pkt);
            usleep(10000);
            r = drmtap_grab_packet(ctx, &pkt);
        }
        double grab_us = now_us() - t0;
        if (r != 0) {
            printf("  frame %d: SKIP: drmtap_grab_packet failed (%d): %s\n", i, r,
                   drmtap_error(ctx) ? drmtap_error(ctx) : "");
            if (i == 0) {
                break;   /* e.g. no active scanout in CI */
            }
            continue;
        }
        /* Forward without looking inside: bytes as the message, fds alongside. */
        int sent = send_msg(sock, pkt.bytes, pkt.len, pkt.fds, pkt.nfds);
        int send_err = errno;
        uint32_t len = pkt.len, nfds = pkt.nfds;
        drmtap_packet_release(&pkt);
        if (sent != 0) {
            /* A converter with no render node answers and exits at once, so the
             * send can fail on a closed peer: its queued answer is still readable. */
            conv_result_t early;
            int efds[DRMTAP_PACKET_MAX_FDS];
            size_t nefds = 0;
            if ((send_err == EPIPE || send_err == ECONNRESET) &&
                recv_msg(sock, &early, sizeof(early), efds, &nefds) == (ssize_t)sizeof(early) &&
                early.ok < 0) {
                printf("  SKIP: the converter has no render node\n");
                break;
            }
            printf("  FAIL: send\n");
            failures++;
            break;
        }
        conv_result_t res;
        int dummy[DRMTAP_PACKET_MAX_FDS];
        size_t nd = 0;
        if (recv_msg(sock, &res, sizeof(res), dummy, &nd) != (ssize_t)sizeof(res)) {
            printf("  FAIL: no result from the converter\n");
            failures++;
            break;
        }
        if (res.ok < 0) {
            printf("  SKIP: the converter has no render node\n");
            break;
        }
        if (res.ok != 1 || res.nonblack == 0) {
            printf("  frame %d: FAIL: convert=%d nonblack=%llu\n", i, res.err,
                   (unsigned long long)res.nonblack);
            failures++;
            continue;
        }
        converted++;
        grab_total += grab_us;
        conv_total += res.convert_us;
        if (frames <= 5 || i == 0 || i == frames - 1) {
            printf("  frame %d: packet %u bytes + %u fds -> %ux%u stride=%u fourcc=%.4s "
                   "nonblack=%llu checksum=%llu (grab %.0f us, convert %.0f us)\n",
                   i, len, nfds, res.width, res.height, res.stride,
                   (const char *)&res.format, (unsigned long long)res.nonblack,
                   (unsigned long long)res.checksum, grab_us, res.convert_us);
        }
    }
    if (converted > 0) {
        printf("  %d frames: mean grab %.0f us, mean convert %.0f us\n", converted,
               grab_total / converted, conv_total / converted);
    }
    drmtap_close(ctx);
    return failures;
}

int main(int argc, char **argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 1;
    if (frames < 1) {
        frames = 1;
    }
    printf("packet_capture: privileged packet export + unprivileged compose, %d frame%s\n",
           frames, frames == 1 ? "" : "s");

    /* A consumer that loads libdrmtap at run time (dlopen) cannot trust the version it
     * was built against. drmtap_abi() says which ABI levels the loaded library serves;
     * the packet API needs level 2. A library without drmtap_abi() is level 1 (0.5.x):
     * a dlopen consumer treats a missing symbol the same way and stays on
     * drmtap_grab_desc() / drmtap_convert_dmabuf(). */
    uint32_t abi_cur = 0, abi_old = 0;
    drmtap_abi(&abi_cur, &abi_old);
    if (abi_old > DRMTAP_ABI_LEVEL || abi_cur < 2) {
        printf("  SKIP: library ABI levels %u..%u do not serve the packet API "
               "(built for level %d)\n", abi_old, abi_cur, DRMTAP_ABI_LEVEL);
        return 0;
    }
    printf("  library ABI levels %u..%u, built for %d\n", abi_old, abi_cur, DRMTAP_ABI_LEVEL);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
        perror("socketpair");
        return 2;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 2;
    }
    if (pid == 0) {
        close(sv[0]);
        int rc = run_converter(sv[1]);
        close(sv[1]);
        _exit(rc);
    }
    close(sv[1]);
    int failures = run_exporter(sv[0], frames);
    close(sv[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        printf("  FAIL: converter killed by signal %d\n", WTERMSIG(status));
        failures++;
    }
    printf("packet_capture: %d failures\n", failures);
    return failures ? 1 : 0;
}
