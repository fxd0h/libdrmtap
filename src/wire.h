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
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>

/* ---- Command framing (library -> helper) --------------------------------
 * A command frame carries a magic + protocol version so a helper and a library
 * built from different releases fail cleanly instead of misreading each other:
 * the helper rejects any frame whose magic/version/length do not match its own
 * build and stops serving, rather than acting on a misparsed frame. The channel
 * is a host-local socketpair between two ends of the same machine, so the multi
 * byte fields are native byte order (both ends share the host endianness); there
 * is no cross-machine transport to byte-swap for. Defined here, once, so the
 * helper, the library client and the tests cannot drift apart. */
#define HELPER_PROTO_MAGIC   0x544D5244u  /* "DRMT" as a little-endian u32 */
#define HELPER_PROTO_VERSION 1u

/* Command types (the `type` field of a command frame). */
#define CMD_GRAB       0x01u
#define CMD_GET_CURSOR 0x02u
#define CMD_QUIT       0xFFu

typedef struct {
    uint32_t magic;    /* HELPER_PROTO_MAGIC */
    uint16_t version;  /* HELPER_PROTO_VERSION */
    uint16_t type;     /* CMD_GRAB / CMD_GET_CURSOR / CMD_QUIT */
    uint32_t length;   /* total frame length in bytes (== sizeof(helper_cmd_grab_t)) */
    uint32_t crtc_id;  /* target CRTC id (0 = auto-select first active) */
} helper_cmd_grab_t;   /* 16 bytes, naturally aligned, no padding */

/* Build a command frame with the header fields set for this build. */
static inline helper_cmd_grab_t wire_cmd(uint16_t type, uint32_t crtc_id) {
    helper_cmd_grab_t c;
    memset(&c, 0, sizeof(c));
    c.magic = HELPER_PROTO_MAGIC;
    c.version = HELPER_PROTO_VERSION;
    c.type = type;
    c.length = (uint32_t)sizeof(helper_cmd_grab_t);
    c.crtc_id = crtc_id;
    return c;
}

/* True only for a valid, version-matched command frame carrying a known command
 * type. A foreign magic, a different protocol version, a length that does not
 * match this build's frame, or a type outside the declared CMD_* set all fail
 * here, so a stale helper/library binary or a malformed command is rejected at
 * one gate before dispatch, not misparsed. */
static inline int wire_cmd_valid(const helper_cmd_grab_t *c) {
    return c->magic == HELPER_PROTO_MAGIC &&
           c->version == HELPER_PROTO_VERSION &&
           c->length == (uint32_t)sizeof(helper_cmd_grab_t) &&
           (c->type == CMD_GRAB || c->type == CMD_GET_CURSOR || c->type == CMD_QUIT);
}

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
