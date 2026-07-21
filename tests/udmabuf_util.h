/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file udmabuf_util.h
 * @brief Shared udmabuf helper for the convert test and the fuzz harness.
 *
 * drmtap_convert_dmabuf() now requires a genuine DMA-BUF (it refuses to mmap an
 * untrusted non-dma-buf fd), so a plain memfd would be rejected. These helpers
 * wrap a sealed memfd in a real, immutable DMA-BUF via /dev/udmabuf, which the
 * converter accepts. Kept in one header so the test and the fuzzer cannot drift.
 *
 * The udmabuf_create struct and ioctl are defined inline so the harnesses do not
 * depend on a linux/udmabuf.h that older build headers lack.
 */

#ifndef DRMTAP_UDMABUF_UTIL_H
#define DRMTAP_UDMABUF_UTIL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/types.h>

#ifndef UDMABUF_CREATE
struct udmabuf_create {
    __u32 memfd;
    __u32 flags;
    __u64 offset;
    __u64 size;
};
#define UDMABUF_CREATE _IOW('u', 0x42, struct udmabuf_create)
#endif

/* Wrap `len` bytes of `data` in a real, immutable DMA-BUF using an already-open
 * /dev/udmabuf control fd. The backing is a page-rounded sealed memfd, so the
 * dma-buf can be smaller than a descriptor's declared geometry (the hostile
 * case the convert path must reject without faulting). Returns the dma-buf fd,
 * or a negative errno (the failing step's errno, preserved across cleanup: e.g.
 * -EINVAL for a bad control fd, or the ftruncate/seal/ioctl error). Does not
 * close `udmabuf_ctrl` (the caller owns it). */
static inline int drmtap_udmabuf_wrap(int udmabuf_ctrl, const void *data, size_t len) {
    if (udmabuf_ctrl < 0) {
        return -EINVAL;
    }
    long pg = sysconf(_SC_PAGESIZE);
    size_t rounded = ((len + (size_t)pg - 1) / (size_t)pg) * (size_t)pg;
    if (rounded == 0) {
        rounded = (size_t)pg;
    }
    int mfd = memfd_create("drmtap-udmabuf", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (mfd < 0) {
        return -errno;
    }
    if (ftruncate(mfd, (off_t)rounded) != 0 ||
        (len && write(mfd, data, len) != (ssize_t)len) ||
        fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) {
        int err = errno ? errno : EIO;  /* a short write may leave errno unset */
        close(mfd);
        return -err;
    }
    struct udmabuf_create create;
    memset(&create, 0, sizeof(create));
    create.memfd = (__u32)mfd;
    create.size = rounded;
    int dbuf = ioctl(udmabuf_ctrl, UDMABUF_CREATE, &create);
    if (dbuf < 0) {
        int err = errno ? errno : EIO;
        close(mfd);
        return -err;
    }
    close(mfd);  /* udmabuf holds its own reference on the pages */
    return dbuf;  /* dma-buf fd */
}

/* Convenience wrapper that opens and closes /dev/udmabuf per call. Returns the
 * dma-buf fd, or a negative errno; -ENOENT/-EACCES (from open) means udmabuf is
 * unavailable and the caller should SKIP. */
static inline int drmtap_udmabuf_make(const void *data, size_t len) {
    int ctrl = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (ctrl < 0) {
        return -errno;  /* udmabuf not available -> caller skips */
    }
    int dbuf = drmtap_udmabuf_wrap(ctrl, data, len);
    close(ctrl);  /* dbuf already holds the result/errno; close cannot change it */
    return dbuf;
}

#endif /* DRMTAP_UDMABUF_UTIL_H */
