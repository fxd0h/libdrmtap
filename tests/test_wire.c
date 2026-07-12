/* Wire-protocol unit tests for the privileged helper IPC.
 *
 * The helper passes a scanout DMA-BUF and fixed-size metadata to an
 * unprivileged peer over a SOCK_STREAM socketpair. These tests exercise the
 * security-relevant invariants the helper and library rely on — a received fd
 * is read-only and close-on-exec, a truncated ancillary message is rejected,
 * and a fragmented / mid-frame-closed fixed-size frame is reassembled or
 * rejected rather than acted on partially — using a fake peer over a
 * socketpair, with NO DRM hardware or privileges. They guard the kernel/wire
 * assumptions the helper.c / privilege_helper.c fixes depend on.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/mman.h>

static int g_failures = 0;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            printf("  PASS: %s\n", (name));                                   \
        } else {                                                              \
            printf("  FAIL: %s (line %d)\n", (name), __LINE__);               \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

/* Mirror of the helper's send_fd: attach one fd via SCM_RIGHTS + meta bytes. */
static int wire_send_fd(int sock, int fd, const void *meta, size_t meta_len) {
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
    return sendmsg(sock, &msg, MSG_NOSIGNAL) < 0 ? -1 : 0;
}

/* Mirror of privilege_helper.c recv_fd_and_meta: MSG_CMSG_CLOEXEC + reject
 * MSG_CTRUNC. `cmsg_cap` lets a test force an undersized ancillary buffer. */
static int wire_recv_fd(int sock, void *meta, size_t meta_len, int *out_fd,
                        size_t cmsg_cap) {
    struct msghdr msg = {0};
    struct iovec iov;
    char cbuf[256];
    *out_fd = -1;
    iov.iov_base = meta;
    iov.iov_len = meta_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = cmsg_cap;
    ssize_t n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    if (n <= 0) {
        return -1;
    }
    if (msg.msg_flags & MSG_CTRUNC) {
        return -1; /* ancillary truncated: fd may have been dropped */
    }
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
        cmsg->cmsg_type == SCM_RIGHTS &&
        cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
        memcpy(out_fd, CMSG_DATA(cmsg), sizeof(int));
    }
    return 0;
}

/* Mirror of recv_all: receive exactly len bytes; 0 ok, -1 on EOF/partial. */
static int wire_recv_all(int sock, void *buf, size_t len) {
    unsigned char *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(sock, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

/* A read-only, size-fixed anonymous fd standing in for a read-only DMA-BUF. */
static int make_readonly_fd(void) {
    int fd = memfd_create("wire-ro", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, 4096) != 0) { close(fd); return -1; }
    /* Seal it write-closed and re-open read-only, mimicking a read-only export. */
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
        close(fd); return -1;
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    int ro = open(path, O_RDONLY | O_CLOEXEC);
    close(fd);
    return ro;
}

static void test_received_fd_is_cloexec(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { CHECK(0, "cloexec: socketpair"); return; }
    int ro = make_readonly_fd();
    if (ro < 0) { CHECK(0, "cloexec: make_readonly_fd"); close(sv[0]); close(sv[1]); return; }
    uint32_t meta = 0xABCD;
    wire_send_fd(sv[0], ro, &meta, sizeof(meta));
    int got_fd = -1; uint32_t got_meta = 0;
    int r = wire_recv_fd(sv[1], &got_meta, sizeof(got_meta), &got_fd, CMSG_SPACE(sizeof(int)));
    CHECK(r == 0 && got_fd >= 0 && got_meta == 0xABCD, "received fd + meta round-trips");
    int flags = got_fd >= 0 ? fcntl(got_fd, F_GETFD) : -1;
    CHECK(flags >= 0 && (flags & FD_CLOEXEC), "received fd is close-on-exec (MSG_CMSG_CLOEXEC)");
    if (got_fd >= 0) close(got_fd);
    close(ro); close(sv[0]); close(sv[1]);
}

static void test_received_fd_is_readonly(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { CHECK(0, "readonly: socketpair"); return; }
    int ro = make_readonly_fd();
    if (ro < 0) { CHECK(0, "readonly: make_readonly_fd"); close(sv[0]); close(sv[1]); return; }
    uint32_t meta = 0;
    wire_send_fd(sv[0], ro, &meta, sizeof(meta));
    int got_fd = -1; uint32_t got_meta = 0;
    wire_recv_fd(sv[1], &got_meta, sizeof(got_meta), &got_fd, CMSG_SPACE(sizeof(int)));
    int accmode = got_fd >= 0 ? (fcntl(got_fd, F_GETFL) & O_ACCMODE) : -1;
    CHECK(accmode == O_RDONLY, "received fd access mode is O_RDONLY");
    void *w = got_fd >= 0 ? mmap(NULL, 4096, PROT_WRITE, MAP_SHARED, got_fd, 0) : MAP_FAILED;
    CHECK(w == MAP_FAILED, "mmap(PROT_WRITE) on the read-only fd is rejected");
    if (w != MAP_FAILED) munmap(w, 4096);
    if (got_fd >= 0) close(got_fd);
    close(ro); close(sv[0]); close(sv[1]);
}

static void test_truncated_ancillary_rejected(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { CHECK(0, "ctrunc: socketpair"); return; }
    int ro = make_readonly_fd();
    if (ro < 0) { CHECK(0, "ctrunc: make_readonly_fd"); close(sv[0]); close(sv[1]); return; }
    uint32_t meta = 0;
    wire_send_fd(sv[0], ro, &meta, sizeof(meta));
    int got_fd = -1; uint32_t got_meta = 0;
    /* Force an ancillary buffer too small to hold the SCM_RIGHTS header: the
     * kernel sets MSG_CTRUNC and drops the fd, and the receiver must reject. */
    int r = wire_recv_fd(sv[1], &got_meta, sizeof(got_meta), &got_fd, sizeof(struct cmsghdr) - 1);
    CHECK(r == -1 && got_fd == -1, "truncated ancillary (MSG_CTRUNC) rejected, no fd leaked");
    close(ro); close(sv[0]); close(sv[1]);
}

static void test_fragmented_frame_reassembled(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { CHECK(0, "fragment: socketpair"); return; }
    unsigned char frame[8] = {0x01, 0, 0, 0, 0xDE, 0xAD, 0xBE, 0xEF};
    /* Send the fixed-size frame in two separate writes. */
    if (send(sv[0], frame, 3, MSG_NOSIGNAL) < 0) { CHECK(0, "fragment: send1"); }
    if (send(sv[0], frame + 3, 5, MSG_NOSIGNAL) < 0) { CHECK(0, "fragment: send2"); }
    unsigned char got[8] = {0};
    int r = wire_recv_all(sv[1], got, sizeof(got));
    CHECK(r == 0 && memcmp(got, frame, sizeof(frame)) == 0,
          "fragmented fixed-size frame reassembled by exact-length recv");
    close(sv[0]); close(sv[1]);
}

static void test_eof_midframe_rejected(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { CHECK(0, "eof: socketpair"); return; }
    unsigned char partial[3] = {0x01, 0, 0};
    if (send(sv[0], partial, sizeof(partial), MSG_NOSIGNAL) < 0) { CHECK(0, "eof: send"); }
    close(sv[0]); /* peer closes mid-frame */
    unsigned char got[8] = {0};
    int r = wire_recv_all(sv[1], got, sizeof(got));
    CHECK(r == -1, "mid-frame EOF rejected (partial command not acted on)");
    close(sv[1]);
}

int main(void) {
    printf("Running wire-protocol tests...\n");
    test_received_fd_is_cloexec();
    test_received_fd_is_readonly();
    test_truncated_ancillary_rejected();
    test_fragmented_frame_reassembled();
    test_eof_midframe_rejected();
    if (g_failures == 0) {
        printf("All wire-protocol tests passed.\n");
        return 0;
    }
    printf("%d wire-protocol test(s) FAILED.\n", g_failures);
    return 1;
}
