/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file split_capture.c
 * @brief End-to-end reference for the split-capture model.
 *
 * Mirrors the privilege split the RustDesk integration uses: the EXPORTER
 * (here, the parent — in production the root `--service`) does a zero-copy
 * grab and ships the DMA-BUF fd + a value descriptor over a unix socket via
 * SCM_RIGHTS; the CONVERTER (the child — in production the unprivileged
 * `--server`) imports the fd on a render node and detiles it to linear RGBA.
 * Neither the GPU stack nor a KMS master ever loads into the exporter.
 *
 * This exercises the real cross-process path: drmtap_grab_desc() (which,
 * unlike drmtap_grab(), captures the CCS aux planes + HDR state a compressed
 * scanout needs) -> SCM_RIGHTS -> drmtap_open_render() + drmtap_convert_dmabuf().
 *
 * Run it on a box with an active scanout. It SKIPs cleanly (exit 0) when no
 * capture device / render node is present.
 *
 *   build/split_capture            # auto-detect
 *   DRM_DEVICE=/dev/dri/card1 build/split_capture
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "drmtap.h"

/* Result the converter reports back to the exporter. */
typedef struct {
    int      ok;
    uint32_t width, height, stride, format;
    uint64_t nonblack;   /* count of non-black (non-zero RGB) pixels */
    uint64_t checksum;   /* cheap content hash so a frozen/garbage frame shows */
} conv_result_t;

/* Send `len` bytes of `data` over `sock`, attaching `fd` via SCM_RIGHTS only
 * when fd >= 0 (passing -1 in an SCM_RIGHTS cmsg is invalid and fails the
 * whole sendmsg — the result messages carry no fd). */
static int send_desc_fd(int sock, const void *data, size_t len, int fd) {
    struct iovec iov = { .iov_base = (void *)data, .iov_len = len };
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    memset(cbuf, 0, sizeof(cbuf));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (fd >= 0) {
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    }
    return sendmsg(sock, &msg, 0) < 0 ? -1 : 0;
}

/* Receive `len` bytes into `data` plus one fd via SCM_RIGHTS (*fd_out = -1 if
 * none arrived). */
static int recv_desc_fd(int sock, void *data, size_t len, int *fd_out) {
    struct iovec iov = { .iov_base = data, .iov_len = len };
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    memset(cbuf, 0, sizeof(cbuf));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);
    ssize_t n = recvmsg(sock, &msg, 0);
    if (n < 0) {
        return -1;
    }
    *fd_out = -1;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            memcpy(fd_out, CMSG_DATA(cm), sizeof(int));
        }
    }
    return (int)n;
}

/* ── CONVERTER (unprivileged child) ── */
static int run_converter(int sock) {
    fprintf(stderr, "converter: started, opening render node...\n");
    fflush(stderr);
    drmtap_ctx *ctx = drmtap_open_render(NULL);
    if (!ctx) {
        fprintf(stderr, "converter: drmtap_open_render failed: %s\n",
                drmtap_error(NULL) ? drmtap_error(NULL) : "(no render node)");
        /* Tell the exporter we could not set up; it will treat it as SKIP. */
        conv_result_t r = { .ok = -1 };
        send_desc_fd(sock, &r, sizeof(r), -1);
        return 0;
    }

    drmtap_dmabuf_desc desc;
    int fd = -1;
    if (recv_desc_fd(sock, &desc, sizeof(desc), &fd) != (int)sizeof(desc)) {
        fprintf(stderr, "converter: short descriptor read\n");
        drmtap_close(ctx);
        return 1;
    }
    desc.dma_buf_fd = fd;  /* the meaningful fd is the one we just received */

    conv_result_t r;
    memset(&r, 0, sizeof(r));
    drmtap_frame_info frame;
    int cret = drmtap_convert_dmabuf(ctx, &desc, &frame);
    if (cret == 0 && frame.data) {
        r.ok = 1;
        r.width = frame.width;
        r.height = frame.height;
        r.stride = frame.stride;
        r.format = frame.format;
        const uint8_t *p = frame.data;
        for (uint32_t y = 0; y < frame.height; y++) {
            const uint8_t *row = p + (size_t)y * frame.stride;
            for (uint32_t x = 0; x < frame.width; x++) {
                const uint8_t *px = row + (size_t)x * 4;
                if (px[0] | px[1] | px[2]) {
                    r.nonblack++;
                }
                r.checksum += (uint64_t)px[0] + px[1] * 3u + px[2] * 7u;
            }
        }
    } else {
        r.ok = 0;
        fprintf(stderr, "converter: drmtap_convert_dmabuf failed (%d): %s\n",
                cret, drmtap_error(ctx) ? drmtap_error(ctx) : "");
    }

    if (fd >= 0) {
        close(fd);
    }
    drmtap_close(ctx);
    send_desc_fd(sock, &r, sizeof(r), -1);
    return 0;
}

/* ── EXPORTER (parent) ── */
static int run_exporter(int sock, pid_t child) {
    drmtap_ctx *ctx = drmtap_open(NULL);
    if (!ctx) {
        printf("  SKIP: no capture device (%s)\n",
               drmtap_error(NULL) ? drmtap_error(NULL) : "drmtap_open failed");
        close(sock);
        waitpid(child, NULL, 0);
        return 0;  /* SKIP */
    }
    printf("  exporter: %s driver=%s\n",
           drmtap_gpu_driver(ctx) ? "opened" : "opened",
           drmtap_gpu_driver(ctx) ? drmtap_gpu_driver(ctx) : "?");

    drmtap_dmabuf_desc desc;
    drmtap_frame_info frame;
    int gret = drmtap_grab_desc(ctx, &desc, &frame);
    if (gret != 0) {
        printf("  SKIP: grab_desc failed (%d): %s\n", gret,
               drmtap_error(ctx) ? drmtap_error(ctx) : "");
        drmtap_close(ctx);
        close(sock);
        waitpid(child, NULL, 0);
        return 0;  /* SKIP — e.g. no active scanout in CI */
    }

    printf("  exporter: grabbed %ux%u fourcc=%.4s modifier=0x%llx "
           "num_planes=%u fb_id=%u hdr_eotf=%u fd=%d\n",
           desc.width, desc.height, (const char *)&desc.format,
           (unsigned long long)desc.modifier, desc.num_planes, desc.fb_id,
           desc.hdr_eotf, desc.dma_buf_fd);
    for (uint32_t p = 0; p < desc.num_planes; p++) {
        printf("           plane[%u]: offset=%u pitch=%u\n",
               p, desc.offsets[p], desc.pitches[p]);
    }
    int is_compressed = (desc.num_planes > 1);
    printf("  exporter: %s scanout -> shipping fd + descriptor over SCM_RIGHTS\n",
           is_compressed ? "COMPRESSED (CCS multi-plane)" : "single-plane");

    int failures = 0;
    if (desc.dma_buf_fd < 0) {
        printf("  NOTE: exporter grab returned no dma_buf_fd (helper path did "
               "not pass one) — cannot exercise the fd round-trip here\n");
    } else if (send_desc_fd(sock, &desc, sizeof(desc), desc.dma_buf_fd) != 0) {
        printf("  FAIL: send_desc_fd\n");
        failures++;
    } else {
        conv_result_t r;
        int dummy = -1;
        if (recv_desc_fd(sock, &r, sizeof(r), &dummy) != (int)sizeof(r)) {
            printf("  FAIL: no result from converter\n");
            failures++;
        } else if (r.ok < 0) {
            printf("  SKIP: converter has no render node\n");
        } else if (r.ok == 1) {
            printf("  converter: OK -> %ux%u stride=%u fourcc=%.4s "
                   "nonblack=%llu/%llu checksum=%llu\n",
                   r.width, r.height, r.stride, (const char *)&r.format,
                   (unsigned long long)r.nonblack,
                   (unsigned long long)((uint64_t)r.width * r.height),
                   (unsigned long long)r.checksum);
            /* The whole point: a real cross-process CCS/linear scanout detiled
             * to non-black linear RGBA proves the split API round-trips. */
            if (r.nonblack == 0) {
                printf("  FAIL: converted frame is all black "
                       "(detile/round-trip broken)\n");
                failures++;
            } else {
                printf("  PASS: cross-process %s round-trip produced a real "
                       "linear frame\n",
                       is_compressed ? "CCS" : "linear");
            }
        } else {
            printf("  FAIL: converter could not convert the shipped buffer\n");
            failures++;
        }
    }

    drmtap_frame_release(ctx, &frame);
    drmtap_close(ctx);
    close(sock);
    int wstatus = 0;
    waitpid(child, &wstatus, 0);
    if (WIFSIGNALED(wstatus)) {
        printf("  DIAG: converter killed by signal %d\n", WTERMSIG(wstatus));
    } else if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0) {
        printf("  DIAG: converter exited %d\n", WEXITSTATUS(wstatus));
    }
    return failures;
}

int main(void) {
    printf("split_capture: privileged export + unprivileged convert round-trip\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
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
    int failures = run_exporter(sv[0], pid);

    printf("split_capture: %d failures\n", failures);
    return failures ? 1 : 0;
}
