/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file fuzz_convert.c
 * @brief libFuzzer target for drmtap_convert_dmabuf(), the split-capture
 *        convert boundary that treats its descriptor as UNTRUSTED IPC input.
 *
 * The fuzzer drives a hostile drmtap_dmabuf_desc (geometry, num_planes,
 * per-plane offsets/pitches, format, modifier, HDR state) together with a REAL
 * dma-buf of arbitrary size (a sealed memfd wrapped by udmabuf, since the
 * convert path now rejects non-dma-buf fds), and forces the CPU mmap +
 * deswizzle/reduce path via DRMTAP_NO_EGL=1 (that is where the untrusted
 * geometry is read). Under ASan, any out-of-bounds access, overflow, or crash
 * on a malformed descriptor is a finding. A well-behaved boundary either
 * returns a negative errno or converts safely for every input.
 *
 * Build: clang -fsanitize=fuzzer,address ... (see fuzz/build.sh)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "drmtap.h"
#include "udmabuf_util.h"

static drmtap_ctx *g_ctx;
static int g_udmabuf = -1;   /* persistent /dev/udmabuf control fd */

__attribute__((constructor))
static void fuzz_init(void) {
    /* Target the CPU convert path (the untrusted-descriptor code): a
     * udmabuf-backed dma-buf is a real dma-buf EGL would otherwise import. */
    setenv("DRMTAP_NO_EGL", "1", 1);
    /* One persistent context for the whole run. Render-only is the split
     * consumer; fall back to a full context so the fuzzer still runs on a box
     * that only exposes a card node. */
    g_ctx = drmtap_open_render(NULL);
    if (!g_ctx) {
        g_ctx = drmtap_open(NULL);
    }
    g_udmabuf = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!g_ctx) {
        return 0;  /* no capture device in this environment */
    }

    struct __attribute__((packed)) fuzz_hdr {
        uint32_t width, height, format;
        uint64_t modifier;
        uint32_t num_planes;
        uint32_t offsets[4];
        uint32_t pitches[4];
        uint32_t hdr_eotf, hdr_max_nits;
    } hdr;
    if (size < sizeof(hdr)) {
        return 0;
    }
    memcpy(&hdr, data, sizeof(hdr));
    const uint8_t *payload = data + sizeof(hdr);
    size_t paylen = size - sizeof(hdr);

    drmtap_dmabuf_desc desc;
    memset(&desc, 0, sizeof(desc));
    /* Bound width/height so a single valid-looking frame cannot allocate the
     * whole 126 MB cap on every call (which would crawl the fuzzer); the
     * library's own validate_fb_size + width*bpp guard are what we exercise. */
    desc.width = hdr.width & 0x1FFF;   /* 0..8191 */
    desc.height = hdr.height & 0x1FFF;
    desc.format = hdr.format;
    desc.modifier = hdr.modifier;
    desc.fb_id = 0;                    /* no cache: import fresh each call */
    desc.num_planes = hdr.num_planes;  /* raw: exercises the >4 rejection */
    for (int i = 0; i < 4; i++) {
        desc.offsets[i] = hdr.offsets[i];
        desc.pitches[i] = hdr.pitches[i];
    }
    desc.hdr_eotf = hdr.hdr_eotf;
    desc.hdr_max_nits = hdr.hdr_max_nits;

    /* A real dma-buf of arbitrary (page-rounded) size: the mmap in the CPU path
     * can be smaller than the geometry claims, which is exactly the hostile
     * case to probe (now caught by the lseek size check, not a fault). */
    int fd = drmtap_udmabuf_wrap(g_udmabuf, payload, paylen);
    if (fd < 0) {
        return 0;
    }
    desc.dma_buf_fd = fd;

    drmtap_frame_info frame;
    (void)drmtap_convert_dmabuf(g_ctx, &desc, &frame);

    close(fd);
    return 0;
}
