/* Wire-protocol unit tests for the privileged helper IPC.
 *
 * These exercise the ACTUAL framing + SCM_RIGHTS primitives from src/wire.h —
 * the same code the helper (helper/drmtap-helper.c) and the library client
 * (src/privilege_helper.c) run — over a fake peer on a socketpair, with NO DRM
 * hardware or privileges. They guard the security-relevant invariants: a
 * received fd is read-only and close-on-exec, a truncated / extra-descriptor
 * ancillary message is rejected, and a fragmented / mid-frame-closed fixed-size
 * frame is reassembled or rejected rather than acted on partially.
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
#include <dirent.h>

#include "../src/wire.h"

/* Count entries under /proc/self/fd. Only the DELTA across a call matters (the
 * transient dir fd + '.'/'..' cancel out), so this detects a descriptor leak. */
static int count_open_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    while (readdir(d) != NULL) n++;
    closedir(d);
    return n;
}

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

/* A read-only, size-fixed anonymous fd standing in for a read-only DMA-BUF. */
static int make_readonly_fd(void) {
    int fd = memfd_create("wire-ro", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, 4096) != 0) { close(fd); return -1; }
    if (fcntl(fd, F_ADD_SEALS,
              F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
        close(fd); return -1;
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    int ro = open(path, O_RDONLY | O_CLOEXEC);
    close(fd);
    return ro;
}

/* Send N fds as ancillary data — used to overflow wire_recv_fd's single-fd
 * control buffer and drive its MSG_CTRUNC rejection path. (CMSG_SPACE(1 int) is
 * 24 bytes on LP64 and CMSG_LEN(2 ints) is also 24, so two fds fit exactly and
 * do NOT truncate; three fds are needed to overflow the receiver's buffer.) */
static int send_n_fds(int sock, const int *fds, int n) {
    struct msghdr msg;
    struct iovec iov;
    char meta = 0;
    char cbuf[CMSG_SPACE(8 * sizeof(int))];
    memset(&msg, 0, sizeof(msg));
    memset(cbuf, 0, sizeof(cbuf));
    iov.iov_base = &meta;
    iov.iov_len = sizeof(meta);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = CMSG_SPACE((size_t)n * sizeof(int));
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN((size_t)n * sizeof(int));
    memcpy(CMSG_DATA(cmsg), fds, (size_t)n * sizeof(int));
    return sendmsg(sock, &msg, MSG_NOSIGNAL) < 0 ? -1 : 0;
}

static void test_received_fd_is_cloexec(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { CHECK(0, "cloexec: socketpair"); return; }
    int ro = make_readonly_fd();
    if (ro < 0) { CHECK(0, "cloexec: make_readonly_fd"); close(sv[0]); close(sv[1]); return; }
    uint32_t meta = 0xABCD;
    wire_send_fd(sv[0], ro, &meta, sizeof(meta));
    int got_fd = -1; uint32_t got_meta = 0;
    int r = wire_recv_fd(sv[1], &got_meta, sizeof(got_meta), &got_fd);
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
    wire_recv_fd(sv[1], &got_meta, sizeof(got_meta), &got_fd);
    int accmode = got_fd >= 0 ? (fcntl(got_fd, F_GETFL) & O_ACCMODE) : -1;
    CHECK(accmode == O_RDONLY, "received fd access mode is O_RDONLY");
    void *w = got_fd >= 0 ? mmap(NULL, 4096, PROT_WRITE, MAP_SHARED, got_fd, 0) : MAP_FAILED;
    CHECK(w == MAP_FAILED, "mmap(PROT_WRITE) on the read-only fd is rejected");
    if (w != MAP_FAILED) munmap(w, 4096);
    if (got_fd >= 0) close(got_fd);
    close(ro); close(sv[0]); close(sv[1]);
}

static void test_extra_descriptor_rejected(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { CHECK(0, "ctrunc: socketpair"); return; }
    int ro = make_readonly_fd();
    if (ro < 0) { CHECK(0, "ctrunc: make_readonly_fd"); close(sv[0]); close(sv[1]); return; }
    /* Send three fds; wire_recv_fd's control buffer holds only one, so the kernel
     * sets MSG_CTRUNC and the receiver must reject rather than take a mismatched
     * fd. Real wire_recv_fd is under test here. */
    int fds[3] = {ro, ro, ro};
    send_n_fds(sv[0], fds, 3);
    char got_meta = 0; int got_fd = -1;
    int before = count_open_fds();
    int r = wire_recv_fd(sv[1], &got_meta, sizeof(got_meta), &got_fd);
    int after = count_open_fds();
    CHECK(r == -1 && got_fd == -1, "extra/truncated ancillary (MSG_CTRUNC) rejected, no fd taken");
    CHECK(before >= 0 && after == before, "no descriptor leaked when a truncated fd message is rejected");
    close(ro); close(sv[0]); close(sv[1]);
}

/* On LP64 CMSG_SPACE(1 int) == CMSG_SPACE(2 ints), so TWO fds fit the one-fd
 * control buffer WITHOUT MSG_CTRUNC. wire_recv_fd must still reject (not exactly
 * one fd) and must close BOTH installed descriptors rather than leak them. */
static void test_two_fds_no_ctrunc_rejected(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { CHECK(0, "twofd: socketpair"); return; }
    int ro = make_readonly_fd();
    if (ro < 0) { CHECK(0, "twofd: make_readonly_fd"); close(sv[0]); close(sv[1]); return; }
    int fds[2] = {ro, ro};
    send_n_fds(sv[0], fds, 2);
    char got_meta = 0; int got_fd = -1;
    int before = count_open_fds();
    int r = wire_recv_fd(sv[1], &got_meta, sizeof(got_meta), &got_fd);
    int after = count_open_fds();
    CHECK(r == -1 && got_fd == -1, "two-fd message with no MSG_CTRUNC is rejected, no fd taken");
    CHECK(before >= 0 && after == before, "no descriptor leaked when a two-fd message is rejected");
    close(ro); close(sv[0]); close(sv[1]);
}

/* A metadata-only message (no SCM_RIGHTS fd) is LEGITIMATE — the V2 pixel path
 * passes no DMA-BUF — so it must succeed with *out_fd == -1, not be rejected as
 * "not exactly one fd". Guards against over-tightening the fd-count check. */
static void test_zero_fds_accepted(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { CHECK(0, "zerofd: socketpair"); return; }
    uint32_t meta = 0xFEEDBEEF;
    if (send(sv[0], &meta, sizeof(meta), MSG_NOSIGNAL) != (ssize_t)sizeof(meta)) {
        CHECK(0, "zerofd: send");
    }
    uint32_t got_meta = 0; int got_fd = -1;
    int r = wire_recv_fd(sv[1], &got_meta, sizeof(got_meta), &got_fd);
    CHECK(r == 0 && got_fd == -1 && got_meta == 0xFEEDBEEF,
          "metadata-only message (no fd) accepted with out_fd = -1");
    close(sv[0]); close(sv[1]);
}

static void test_fragmented_frame_reassembled(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { CHECK(0, "fragment: socketpair"); return; }
    unsigned char frame[8] = {0x01, 0, 0, 0, 0xDE, 0xAD, 0xBE, 0xEF};
    if (wire_send_all(sv[0], frame, 3) != 0) { CHECK(0, "fragment: send1"); }
    if (wire_send_all(sv[0], frame + 3, 5) != 0) { CHECK(0, "fragment: send2"); }
    unsigned char got[8] = {0};
    int r = wire_recv_all(sv[1], got, sizeof(got));
    CHECK(r == 0 && memcmp(got, frame, sizeof(frame)) == 0,
          "fragmented fixed-size frame reassembled by wire_recv_all");
    close(sv[0]); close(sv[1]);
}

static void test_eof_midframe_rejected(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { CHECK(0, "eof: socketpair"); return; }
    unsigned char partial[3] = {0x01, 0, 0};
    if (wire_send_all(sv[0], partial, sizeof(partial)) != 0) { CHECK(0, "eof: send"); }
    close(sv[0]); /* peer closes mid-frame */
    unsigned char got[8] = {0};
    int r = wire_recv_all(sv[1], got, sizeof(got));
    CHECK(r == -1, "mid-frame EOF rejected by wire_recv_all (partial command not acted on)");
    close(sv[1]);
}

/* The command frame carries a magic + protocol version so a helper and a library
 * built from different releases fail cleanly. wire_cmd() builds a valid frame and
 * wire_cmd_valid() (the check the helper runs before acting on a frame) must
 * accept it and reject a foreign magic, a different version, or a wrong length. */
static void test_cmd_frame_magic_version(void) {
    helper_cmd_grab_t c = wire_cmd(CMD_GRAB, 42);
    CHECK(sizeof(helper_cmd_grab_t) == 16, "command frame is 16 bytes (fixed wire layout)");
    CHECK(c.magic == HELPER_PROTO_MAGIC && c.version == HELPER_PROTO_VERSION &&
          c.length == sizeof(helper_cmd_grab_t) && c.type == CMD_GRAB && c.crtc_id == 42,
          "wire_cmd sets magic/version/length/type/crtc_id");
    CHECK(wire_cmd_valid(&c), "well-formed command frame accepted");

    /* Round-trip over a socketpair, exactly as the helper reads it. */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { CHECK(0, "cmd: socketpair"); return; }
    helper_cmd_grab_t sent = wire_cmd(CMD_GET_CURSOR, 7);
    CHECK(wire_send_all(sv[0], &sent, sizeof(sent)) == 0, "command frame sent");
    helper_cmd_grab_t got;
    memset(&got, 0, sizeof(got));
    int r = wire_recv_all(sv[1], &got, sizeof(got));
    CHECK(r == 0 && wire_cmd_valid(&got) && got.type == CMD_GET_CURSOR && got.crtc_id == 7,
          "received command frame is valid and preserves type/crtc_id");
    close(sv[0]); close(sv[1]);

    /* A foreign magic, a bumped version, and a wrong length are each rejected. */
    helper_cmd_grab_t bad = wire_cmd(CMD_GRAB, 0);
    bad.magic ^= 0xFFFFFFFFu;
    CHECK(!wire_cmd_valid(&bad), "foreign magic rejected");
    bad = wire_cmd(CMD_GRAB, 0); bad.version = (uint16_t)(HELPER_PROTO_VERSION + 1);
    CHECK(!wire_cmd_valid(&bad), "mismatched protocol version rejected");
    bad = wire_cmd(CMD_GRAB, 0); bad.length = sizeof(helper_cmd_grab_t) + 1;
    CHECK(!wire_cmd_valid(&bad), "wrong frame length rejected");
    bad = wire_cmd(CMD_GRAB, 0); bad.type = 0x7Fu; /* not a declared CMD_* */
    CHECK(!wire_cmd_valid(&bad), "unknown command type rejected");
}

int main(void) {
    printf("Running wire-protocol tests (against src/wire.h)...\n");
    test_received_fd_is_cloexec();
    test_received_fd_is_readonly();
    test_extra_descriptor_rejected();
    test_two_fds_no_ctrunc_rejected();
    test_zero_fds_accepted();
    test_fragmented_frame_reassembled();
    test_eof_midframe_rejected();
    test_cmd_frame_magic_version();
    if (g_failures == 0) {
        printf("All wire-protocol tests passed.\n");
        return 0;
    }
    printf("%d wire-protocol test(s) FAILED.\n", g_failures);
    return 1;
}
