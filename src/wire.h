/* SPDX-License-Identifier: MIT
 *
 * wire.h — the fixed-size framing + SCM_RIGHTS primitives shared by the helper
 * (helper/drmtap-helper.c), the library-side client (src/privilege_helper.c),
 * and the wire-protocol tests (tests/test_wire.c). Keeping one implementation
 * here means the tests exercise the SAME code the helper IPC runs, and the two
 * ends cannot drift apart.
 *
 * Contract:
 *   - wire_recv_all / wire_send_all: transfer exactly `len` bytes, retrying on
 *     partial transfers and EINTR; return 0 on success, -1 on EOF/error. A
 *     truncated fixed-size frame is thus rejected, never acted on partially.
 *   - wire_send_fd: attach one fd via SCM_RIGHTS to `meta`, finishing any
 *     partial metadata write so the receiver is never left waiting.
 *   - wire_recv_fd: receive `meta` + at most one fd, set close-on-exec on the
 *     fd ATOMICALLY (MSG_CMSG_CLOEXEC), and reject a truncated control message
 *     (MSG_CTRUNC) so a dropped/extra descriptor cannot slip through.
 */
#ifndef DRMTAP_WIRE_H
#define DRMTAP_WIRE_H

#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>

/* Send exactly len bytes, handling partial writes and EINTR. 0 ok, -1 error. */
static inline int wire_send_all(int sock, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            return -1; /* peer gone */
        }
        sent += (size_t)n;
    }
    return 0;
}

/* Receive exactly len bytes, handling partial reads and EINTR. Returns 0 on a
 * complete read, -1 on EOF/error/partial — so a truncated command frame is
 * rejected rather than acted on with uninitialized fields. */
static inline int wire_recv_all(int sock, void *buf, size_t len) {
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(sock, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            return -1; /* peer closed (clean at a boundary, or mid-frame) */
        }
        got += (size_t)n;
    }
    return 0;
}

/* Send one fd via SCM_RIGHTS together with meta bytes. sendmsg attaches the fd
 * to the first byte but may write fewer than meta_len metadata bytes; finish
 * the remainder so the receiver is not left waiting on a partial frame while we
 * report success. Returns 0 on success, -1 on error. */
static inline int wire_send_fd(int sock, int fd, const void *meta, size_t meta_len) {
    struct msghdr msg;
    struct iovec iov;
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } cmsg_buf;
    memset(&msg, 0, sizeof(msg));
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

    ssize_t n;
    do {
        n = sendmsg(sock, &msg, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        return -1;
    }
    if ((size_t)n < meta_len) {
        return wire_send_all(sock, (const unsigned char *)meta + (size_t)n,
                             meta_len - (size_t)n);
    }
    return 0;
}

/* Receive meta bytes + at most one fd via SCM_RIGHTS. MSG_CMSG_CLOEXEC sets
 * FD_CLOEXEC atomically on any received fd (a plain recvmsg + fcntl would race
 * in a multithreaded process). A truncated control message (MSG_CTRUNC) — an
 * undersized ancillary buffer or an unexpected extra descriptor — is rejected
 * so a possibly-mismatched fd/metadata pair is never used. *out_fd is -1 when
 * no fd was received. Returns 0 on success, -1 on EOF/error. */
static inline int wire_recv_fd(int sock, void *meta_buf, size_t meta_len, int *out_fd) {
    struct msghdr msg;
    struct iovec iov;
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } cmsg_buf;
    *out_fd = -1;
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = meta_buf;
    iov.iov_len = meta_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf.buf;
    msg.msg_controllen = sizeof(cmsg_buf.buf);

    ssize_t n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    if (n <= 0) {
        return -1;
    }

    /* Collect every descriptor the kernel installed across all SCM_RIGHTS
     * blocks. At most one fd is expected; zero is legitimate (a metadata-only
     * message, e.g. the V2 pixel path that passes no DMA-BUF), but two or more —
     * or any truncation — is a protocol violation. Every received descriptor
     * beyond the first is closed immediately, and on a reject the first is
     * closed too, so no path leaks a descriptor. This matters because on LP64
     * CMSG_SPACE is identical for one and two ints, so a peer can slip TWO fds
     * into the one-fd control buffer with NO MSG_CTRUNC, and the truncated path
     * can still install the fds that fit before the kernel raised MSG_CTRUNC. */
    int accepted = -1;
    int nfds = 0;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len < CMSG_LEN(0)) {
            continue;
        }
        size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; i++) {
            int fd;
            memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
            if (nfds == 0) {
                accepted = fd; /* keep the first; may still be rejected below */
            } else {
                close(fd);     /* never keep an unexpected extra descriptor */
            }
            nfds++;
        }
    }
    if ((msg.msg_flags & MSG_CTRUNC) || nfds > 1) {
        if (accepted >= 0) {
            close(accepted);
        }
        return -1;
    }
    /* nfds is 0 (accepted stays -1) or 1 (accepted holds the single fd). */
    *out_fd = accepted;

    /* A short first read is possible; read the metadata remainder. */
    if ((size_t)n < meta_len) {
        if (wire_recv_all(sock, (unsigned char *)meta_buf + n, meta_len - (size_t)n) < 0) {
            close(*out_fd);
            *out_fd = -1;
            return -1;
        }
    }
    return 0;
}

#endif /* DRMTAP_WIRE_H */
