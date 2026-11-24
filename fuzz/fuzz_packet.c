/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file fuzz_packet.c
 * @brief libFuzzer target for drmtap_convert_packet(), the converter side of the
 *        multi-plane split capture. Its bytes and fds arrive from another process
 *        and are UNTRUSTED.
 *
 * A random byte string almost never gets past the magic, so the input is applied
 * to a VALID two-layer packet instead: input[0] picks how many of four real
 * dma-bufs (udmabuf, of different sizes) go with it, input[1] shifts the packet
 * length, and the rest is XORed over the packet bytes. Most inputs are then a
 * near-valid packet with a few hostile fields, which is the case that reaches
 * the fd size checks and the CPU composer. DRMTAP_NO_EGL=1 keeps it on the CPU
 * path (a GPU import of garbage layouts is the driver's business, not ours).
 * Under ASan any read outside a buffer, overflow or fault is a finding.
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
#include "layers.h"
#include "udmabuf_util.h"

static drmtap_ctx *g_ctx;
static int g_fds[4] = { -1, -1, -1, -1 };
static uint8_t g_tmpl[DRMTAP_PKT_MAX_LEN];
static int g_tmpl_len;

__attribute__((constructor))
static void fuzz_init(void) {
    setenv("DRMTAP_NO_EGL", "1", 1);
    g_ctx = drmtap_open_render(NULL);
    if (!g_ctx) {
        g_ctx = drmtap_open(NULL);
    }
    int ctrl = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    static const size_t sizes[4] = { 64 * 48 * 4, 16 * 16 * 4, 4096, 1 << 20 };
    for (int i = 0; i < 4 && ctrl >= 0; i++) {
        uint8_t *p = calloc(1, sizes[i]);
        if (p) {
            memset(p, 0x5a + i, sizes[i]);
            g_fds[i] = drmtap_udmabuf_wrap(ctrl, p, sizes[i]);
            free(p);
        }
    }
    if (ctrl >= 0) {
        close(ctrl);
    }
    drmtap_layer_set s;
    memset(&s, 0, sizeof(s));
    s.crtc_w = 64; s.crtc_h = 48; s.count = 2; s.nfds = 2;
    for (int i = 0; i < 2; i++) {
        drmtap_layer *l = &s.layers[i];
        uint32_t w = i ? 16 : 64, h = i ? 16 : 48;
        l->plane_id = 10 + i;
        l->fb_id = 0;
        l->fb_width = w;
        l->fb_height = h;
        l->format = i ? 0x34325241u : 0x34325258u;   /* AR24 over XR24 */
        l->modifier = 0;
        l->num_planes = 1;
        l->pitches[0] = w * 4;
        l->fd_index[0] = (uint8_t)i;
        l->fd_index[1] = l->fd_index[2] = l->fd_index[3] = DRMTAP_PKT_NO_FD;
        l->src_w = w << 16;
        l->src_h = h << 16;
        l->crtc_x = i ? 5 : 0;
        l->crtc_y = i ? 7 : 0;
        l->crtc_w = w;
        l->crtc_h = h;
        l->zpos = (uint32_t)i;
        l->alpha = 0xffff;
        l->rotation = DRMTAP_ROT_0;
    }
    g_tmpl_len = drmtap_packet_encode(&s, g_tmpl, sizeof(g_tmpl));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!g_ctx || g_tmpl_len <= 0 || g_fds[0] < 0 || size < 2) {
        return 0;
    }
    size_t nfds = data[0] % 5;
    int len = g_tmpl_len + (int8_t)data[1];
    if (len < 0) {
        len = 0;
    }
    if (len > DRMTAP_PACKET_MAX_BYTES) {
        len = DRMTAP_PACKET_MAX_BYTES;
    }
    uint8_t *pkt = malloc(len ? (size_t)len : 1);   /* exact size: ASan sees overreads */
    if (!pkt) {
        return 0;
    }
    memset(pkt, 0, len ? (size_t)len : 1);
    memcpy(pkt, g_tmpl, (size_t)len < (size_t)g_tmpl_len ? (size_t)len : (size_t)g_tmpl_len);
    for (size_t i = 2; i < size && len > 0; i++) {
        pkt[(i - 2) % (size_t)len] ^= data[i];
    }
    drmtap_frame_info frame;
    (void)drmtap_convert_packet(g_ctx, pkt, (size_t)len, g_fds, nfds, &frame);
    free(pkt);
    return 0;
}
