/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file test_layers.c
 * @brief Multi-plane scanout (0.6.0): layer ordering, rectangle math, the packet
 *        codec against hostile input, the CPU composer, and the GPU composer
 *        checked against the CPU one on real (udmabuf) DMA-BUFs.
 *
 * The pure parts need no hardware. The convert_packet part needs /dev/udmabuf
 * and a render node, and SKIPs without them; its EGL half SKIPs when the render
 * node has no usable EGL.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "drmtap.h"
#include "drmtap_internal.h"
#include "udmabuf_util.h"

static int failures = 0;

static void check(int cond, const char *what) {
    if (cond) {
        printf("  OK: %s\n", what);
    } else {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

/* DRM fourccs (no libdrm header in a unit test) */
#define FMT_XR24 0x34325258u /* XRGB8888 */
#define FMT_AR24 0x34325241u /* ARGB8888 */
#define FMT_AB24 0x34324241u /* ABGR8888 */
#define MOD_LINEAR 0ull

static drmtap_layer mk_layer(uint32_t plane, uint32_t fb_w, uint32_t fb_h, int32_t x, int32_t y,
                             uint32_t w, uint32_t h, uint32_t z) {
    drmtap_layer l;
    memset(&l, 0, sizeof(l));
    l.plane_id = plane;
    l.fb_id = plane + 100;
    l.fb_width = fb_w;
    l.fb_height = fb_h;
    l.format = FMT_XR24;
    l.modifier = MOD_LINEAR;
    l.num_planes = 1;
    l.pitches[0] = fb_w * 4;
    l.fd_index[0] = 0;
    l.fd_index[1] = l.fd_index[2] = l.fd_index[3] = DRMTAP_PKT_NO_FD;
    l.src_w = fb_w << 16;
    l.src_h = fb_h << 16;
    l.crtc_x = x;
    l.crtc_y = y;
    l.crtc_w = w;
    l.crtc_h = h;
    l.zpos = z;
    l.alpha = 0xffff;
    l.blend = DRMTAP_BLEND_PREMULTI;
    l.rotation = DRMTAP_ROT_0;
    return l;
}

static int feq(float a, float b) {
    float d = a - b;
    return d < 1e-5f && d > -1e-5f;
}

/* ===== ordering and geometry ===== */

static void test_sort_and_cover(void) {
    printf("test_sort_and_cover:\n");
    drmtap_layer l[4];
    l[0] = mk_layer(134, 10, 10, 0, 0, 10, 10, 4);
    l[1] = mk_layer(34, 10, 10, 0, 0, 10, 10, 0);
    l[2] = mk_layer(94, 10, 10, 0, 0, 10, 10, 2);
    l[3] = mk_layer(64, 10, 10, 0, 0, 10, 10, 2);
    drmtap_layers_sort(l, 4);
    check(l[0].plane_id == 34 && l[1].plane_id == 64 && l[2].plane_id == 94 &&
          l[3].plane_id == 134, "bottom to top by zpos, ties by plane id");

    drmtap_layer c = mk_layer(1, 1920, 1080, 0, 0, 1920, 1080, 0);
    check(drmtap_layer_covers(&c, 1920, 1080), "full-CRTC layer covers");
    drmtap_layer u = mk_layer(1, 936, 702, 492, 180, 936, 702, 0);
    check(!drmtap_layer_covers(&u, 1920, 1080), "the 936x702 underlay window does not cover");
    drmtap_layer o = mk_layer(1, 2000, 1200, -10, -10, 2000, 1200, 0);
    check(drmtap_layer_covers(&o, 1920, 1080), "a layer hanging off every edge covers");
}

static void test_quad(void) {
    printf("test_quad:\n");
    float pos[8], tex[8];
    /* the measured underlay: 936x702 window at +492+180 on a 1920x1080 CRTC */
    drmtap_layer u = mk_layer(1, 936, 702, 492, 180, 936, 702, 0);
    drmtap_layer_quad(&u, 1920, 1080, pos, tex);
    check(feq(pos[0], 2.0f * 492 / 1920 - 1) && feq(pos[1], 2.0f * 180 / 1080 - 1) &&
          feq(pos[6], 2.0f * 1428 / 1920 - 1) && feq(pos[7], 2.0f * 882 / 1080 - 1),
          "destination corners in NDC, y=-1 at image row 0");
    check(feq(tex[0], 0) && feq(tex[1], 0) && feq(tex[6], 1) && feq(tex[7], 1),
          "whole-fb source maps to 0..1");

    /* a source sub-rectangle, 16.16 */
    drmtap_layer s = mk_layer(1, 100, 50, 0, 0, 10, 10, 0);
    s.src_x = 10u << 16; s.src_y = 5u << 16; s.src_w = 40u << 16; s.src_h = 20u << 16;
    drmtap_layer_quad(&s, 10, 10, pos, tex);
    check(feq(tex[0], 0.1f) && feq(tex[1], 0.1f) && feq(tex[6], 0.5f) && feq(tex[7], 0.5f),
          "SRC sub-rect becomes texcoords 0.1..0.5");
    s.src_x = (10u << 16) | 0x8000u;   /* 10.5 */
    drmtap_layer_quad(&s, 10, 10, pos, tex);
    check(feq(tex[0], 10.5f / 100.0f), "fractional SRC_X kept");

    /* rotations: TL,TR,BL,BR of the destination take these source corners */
    drmtap_layer r = mk_layer(1, 4, 2, 0, 0, 2, 4, 0);
    r.rotation = DRMTAP_ROT_90;
    drmtap_layer_quad(&r, 2, 4, pos, tex);
    check(feq(tex[0], 1) && feq(tex[1], 0) && feq(tex[4], 0) && feq(tex[5], 0) &&
          feq(tex[2], 1) && feq(tex[3], 1) && feq(tex[6], 0) && feq(tex[7], 1),
          "rotate-90 (counter-clockwise): dst TL<-src TR, BL<-TL, TR<-BR, BR<-BL");
    r.rotation = DRMTAP_ROT_180;
    drmtap_layer_quad(&r, 2, 4, pos, tex);
    check(feq(tex[0], 1) && feq(tex[1], 1) && feq(tex[6], 0) && feq(tex[7], 0),
          "rotate-180: dst TL<-src BR");
    r.rotation = DRMTAP_ROT_270;
    drmtap_layer_quad(&r, 2, 4, pos, tex);
    check(feq(tex[0], 0) && feq(tex[1], 1) && feq(tex[2], 0) && feq(tex[3], 0),
          "rotate-270: dst TL<-src BL, TR<-TL");
    r.rotation = DRMTAP_ROT_0 | DRMTAP_REFLECT_X;
    drmtap_layer_quad(&r, 2, 4, pos, tex);
    check(feq(tex[0], 1) && feq(tex[1], 0) && feq(tex[2], 0) && feq(tex[3], 0),
          "reflect-x mirrors left/right");
    r.rotation = DRMTAP_ROT_0 | DRMTAP_REFLECT_Y;
    drmtap_layer_quad(&r, 2, 4, pos, tex);
    check(feq(tex[0], 0) && feq(tex[1], 1) && feq(tex[4], 0) && feq(tex[5], 0),
          "reflect-y mirrors top/bottom");

    check(drmtap_rotation_valid(DRMTAP_ROT_0) && drmtap_rotation_valid(0x22) &&
          !drmtap_rotation_valid(0) && !drmtap_rotation_valid(0x3) &&
          !drmtap_rotation_valid(0x41), "rotation validity: one rotate bit, known bits only");
}

static void test_key(void) {
    printf("test_key:\n");
    drmtap_layer_set a;
    memset(&a, 0, sizeof(a));
    a.crtc_w = 1920; a.crtc_h = 1080; a.count = 2;
    a.layers[0] = mk_layer(34, 1920, 1080, 0, 0, 1920, 1080, 0);
    a.layers[1] = mk_layer(134, 250, 250, 835, 392, 250, 250, 4);
    drmtap_layer_set b = a;
    check(drmtap_layers_key(&a) == drmtap_layers_key(&b) && drmtap_layers_same(&a, &b),
          "same layers, same key");
    b.layers[1].fb_id++;
    check(drmtap_layers_key(&a) != drmtap_layers_key(&b) && !drmtap_layers_same(&a, &b),
          "a flip of one layer changes the key");
    b = a;
    b.layers[1].crtc_x++;
    check(drmtap_layers_key(&a) != drmtap_layers_key(&b) && !drmtap_layers_same(&a, &b),
          "a moved window changes the key");
}

static void test_underlay_timing(void) {
    printf("test_underlay_timing:\n");
    int64_t since = 0;
    const int64_t L = DRMTAP_UNDERLAY_EAGAIN_MS;
    check(drmtap_underlay_decide(&since, 1000, 0, 1, L) == DRMTAP_UNDERLAY_NONE && since == 0,
          "no underlay: normal path");
    check(drmtap_underlay_decide(&since, 5000, 1, 1, L) == DRMTAP_UNDERLAY_EAGAIN && since == 5000,
          "underlay starts: -EAGAIN, start remembered");
    check(drmtap_underlay_decide(&since, 5000 + L - 1, 1, 1, L) == DRMTAP_UNDERLAY_EAGAIN,
          "still -EAGAIN 1 ms before the limit");
    check(drmtap_underlay_decide(&since, 5000 + L, 1, 1, L) == DRMTAP_UNDERLAY_COVER,
          "at the limit: the covering layer");
    check(drmtap_underlay_decide(&since, 5000 + 10 * L, 1, 1, L) == DRMTAP_UNDERLAY_COVER,
          "and for as long as the underlay lasts");
    check(drmtap_underlay_decide(&since, 5000 + 10 * L, 1, 0, L) == DRMTAP_UNDERLAY_EAGAIN,
          "no covering layer: -EAGAIN goes on");
    check(drmtap_underlay_decide(&since, 5000 + 11 * L, 0, 1, L) == DRMTAP_UNDERLAY_NONE && since == 0,
          "underlay over: reset");
    check(drmtap_underlay_decide(&since, 5000 + 11 * L + 1, 1, 1, L) == DRMTAP_UNDERLAY_EAGAIN,
          "a new underlay starts the 2 s again");
    since = 0;
    check(drmtap_underlay_decide(&since, 0, 1, 1, L) == DRMTAP_UNDERLAY_EAGAIN && since != 0,
          "a clock reading 0 still marks the start");
}

/* grab_desc's covering layer after a long underlay is narrowed from ITS rect: a
 * padded 64-wide fb scanned out 60 wide is reported 60, whatever the (window-sized)
 * primary plane says. */
static void test_cover_rect(void) {
    printf("test_cover_rect:\n");
    drmtap_layer cover = mk_layer(134, 64, 2170, 0, 0, 60, 2170, 4);
    cover.src_w = 60u << 16;
    drmtap_plane_rect r;
    memset(&r, 0, sizeof(r));
    drmtap_layer_plane_rect(&cover, &r);
    drmtap_scanout_why why = DRMTAP_SCANOUT_AS_IS;
    check(r.valid && r.src_w == 60 && r.crtc_w == 60 &&
          drmtap_scanout_width_of(64, &r, 1, &why) == 60 && why == DRMTAP_SCANOUT_NARROWED,
          "the covering layer rect narrows its padded fb to 60");
    drmtap_layer win = mk_layer(34, 20, 30, 10, 10, 20, 30, 0);
    memset(&r, 0, sizeof(r));
    drmtap_layer_plane_rect(&win, &r);
    check(drmtap_scanout_width_of(64, &r, 1, &why) == 20,
          "the window-sized primary rect would have cut the desktop to 20 columns");
    cover.src_x = (2u << 16) | 0x8000u;
    drmtap_layer_plane_rect(&cover, &r);
    check(r.src_x == 2, "16.16 SRC is truncated to whole pixels, like the property reader");
}

static void test_stand_in(void) {
    printf("test_stand_in:\n");
    drmtap_layer_set s;
    memset(&s, 0, sizeof(s));
    s.count = 3;
    s.layers[0] = mk_layer(10, 64, 48, 0, 0, 64, 48, 0);
    s.layers[0].flags = DRMTAP_LAYER_FLAG_PRIMARY;
    s.layers[1] = mk_layer(11, 16, 16, 5, 5, 16, 16, 1);
    s.layers[2] = mk_layer(12, 64, 48, 0, 0, 64, 48, 2);
    check(drmtap_layers_stand_in(&s, 64, 48) == 0, "a covering primary stands for the screen");
    s.layers[0] = mk_layer(10, 16, 16, 10, 10, 16, 16, 0);   /* underlay: primary = window */
    s.layers[0].flags = DRMTAP_LAYER_FLAG_PRIMARY;
    s.layers[1] = mk_layer(11, 64, 48, 0, 0, 64, 48, 1);     /* covers too, but lower */
    check(drmtap_layers_stand_in(&s, 64, 48) == 2, "in an underlay the topmost covering layer does");
    s.layers[1] = mk_layer(11, 16, 16, 5, 5, 16, 16, 1);
    s.layers[2] = mk_layer(12, 32, 48, 0, 0, 32, 48, 2);
    check(drmtap_layers_stand_in(&s, 64, 48) == -1, "no covering layer: none does");
}
static void test_packet_init(void) {
    printf("test_packet_init:\n");
    drmtap_packet p;
    memset(&p, 0x5a, sizeof(p));
    check(drmtap_grab_packet(NULL, &p) == -EINVAL && p.nfds == 0 && p.len == 0 &&
          p.fds[0] == -1 && p.fds[DRMTAP_PACKET_MAX_FDS - 1] == -1,
          "grab_packet(NULL ctx) leaves an empty packet that is safe to release");
    drmtap_packet_release(&p);
    drmtap_packet_release(NULL);
}

/* ===== packet codec ===== */

static drmtap_layer_set sample_set(void) {
    drmtap_layer_set s;
    memset(&s, 0, sizeof(s));
    s.crtc_w = 1920; s.crtc_h = 1080; s.count = 2; s.lit = 2; s.nfds = 2;
    s.hdr_eotf = 2; s.hdr_max_nits = 1000;
    s.layers[0] = mk_layer(34, 936, 702, 492, 180, 936, 702, 0);
    s.layers[0].flags = DRMTAP_LAYER_FLAG_PRIMARY;
    s.layers[0].format = FMT_AR24;
    s.layers[0].modifier = 0x10000000000000full;
    s.layers[0].num_planes = 3;
    s.layers[0].fd_index[1] = 0; s.layers[0].fd_index[2] = 0;
    s.layers[0].offsets[1] = 3000000; s.layers[0].pitches[1] = 512;
    s.layers[0].offsets[2] = 3100000; s.layers[0].pitches[2] = 64;
    s.layers[1] = mk_layer(134, 1920, 1080, 0, 0, 1920, 1080, 4);
    s.layers[1].fd_index[0] = 1;
    s.layers[1].crtc_x = -3;
    s.layers[1].alpha = 0x8000;
    s.layers[1].blend = DRMTAP_BLEND_COVERAGE;
    s.layers[1].rotation = DRMTAP_ROT_180 | DRMTAP_REFLECT_X;
    s.layers[1].color_encoding = DRMTAP_YCBCR_BT709;
    s.layers[1].color_range = DRMTAP_RANGE_FULL;
    return s;
}

static void test_packet_roundtrip(void) {
    printf("test_packet_roundtrip:\n");
    drmtap_layer_set s = sample_set(), d;
    uint8_t buf[DRMTAP_PKT_MAX_LEN];
    int len = drmtap_packet_encode(&s, buf, sizeof(buf));
    check(len == (int)(DRMTAP_PKT_HDR_LEN + 2 * DRMTAP_PKT_LAYER_LEN), "two layers encode to 292 bytes");
    check(drmtap_packet_decode(buf, (size_t)len, 2, &d) == 0, "and decode");
    int same = d.count == 2 && d.crtc_w == 1920 && d.crtc_h == 1080 && d.nfds == 2 &&
               d.hdr_eotf == 2 && d.hdr_max_nits == 1000 && d.fds[0] == -1;
    for (uint32_t i = 0; i < 2 && same; i++) {
        same = memcmp(&s.layers[i], &d.layers[i], sizeof(drmtap_layer)) == 0;
    }
    check(same, "every field survives the round trip (negative crtc_x, enums, 64-bit modifier)");
    check(drmtap_packet_encode(&s, buf, 100) == -ENOSPC, "encode refuses a short buffer");
    s.count = 0;
    check(drmtap_packet_encode(&s, buf, sizeof(buf)) == -EINVAL, "encode refuses an empty set");
    check(DRMTAP_PKT_MAX_LEN <= DRMTAP_PACKET_MAX_BYTES, "a full packet fits drmtap_packet");
}

typedef void (*mutate_fn)(uint8_t *b, size_t *len, size_t *nfds);
#define L0 (DRMTAP_PKT_HDR_LEN)
#define L1 (DRMTAP_PKT_HDR_LEN + DRMTAP_PKT_LAYER_LEN)
static void put32(uint8_t *p, uint32_t v) { drmtap_pkt_put32(p, v); }
static void m_short(uint8_t *b, size_t *l, size_t *n) { (void)b; (void)n; *l = 20; }
static void m_trunc(uint8_t *b, size_t *l, size_t *n) { (void)b; (void)n; *l -= 1; }
static void m_extra(uint8_t *b, size_t *l, size_t *n) { (void)n; b[*l] = 0; *l += 1; }
static void m_magic(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; b[0] ^= 1; }
static void m_version(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; b[4] = 2; }
static void m_hlen(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; b[6] = 40; }
static void m_total(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + 8, 1000); }
static void m_count0(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; b[12] = 0; }
static void m_count5(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; b[12] = 5; }
static void m_count1(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; b[12] = 1; }
static void m_rlen(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; b[14] = 64; }
static void m_nfds_decl(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; b[16] = 3; }
static void m_nfds_given(uint8_t *b, size_t *l, size_t *n) { (void)b; (void)l; *n = 1; }
static void m_nfds_huge(uint8_t *b, size_t *l, size_t *n) { (void)l; b[16] = 200; *n = 200; }
static void m_flags(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; b[18] = 1; }
static void m_crtc0(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + 20, 0); }
static void m_crtcbig(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + 24, 0xffffffffu); }
static void m_fbw0(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L0 + 8, 0); }
static void m_fbhbig(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L0 + 12, 40000); }
static void m_np0(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L0 + 20, 0); }
static void m_np5(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 20, 5); }
static void m_pitch0(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 48, 0); }
static void m_offneg(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L0 + 36, 0x80000000u); }
static void m_fdidx(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; b[L1 + 64] = 2; }
static void m_fdunused(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; b[L1 + 65] = 0; }
static void m_pitchunused(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 52, 4); }
static void m_srcx(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 68, 1u << 16); }
static void m_srcw0(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 76, 0); }
static void m_srcwrap(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 68, 0xffff0000u); }
static void m_crtcw0(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 92, 0); }
static void m_crtcxbig(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 84, 0x80000000u); }
static void m_alpha(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 104, 0x10000); }
static void m_blend(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 108, 3); }
static void m_rot0(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 112, 0); }
static void m_rot2(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 112, 3); }
static void m_rotx(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 112, 0x41); }
static void m_enc(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 116, 3); }
static void m_range(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 120, 2); }
static void m_lflags(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 124, 2); }
static void m_order(uint8_t *b, size_t *l, size_t *n) { (void)l; (void)n; put32(b + L1 + 100, 0); put32(b + L1 + 0, 1); }

static void test_packet_malformed(void) {
    printf("test_packet_malformed:\n");
    static const struct { mutate_fn f; int want; const char *what; } cases[] = {
        { m_short, -EINVAL, "shorter than a header" },
        { m_trunc, -EINVAL, "one byte short" },
        { m_extra, -EINVAL, "one byte too many" },
        { m_magic, -EPROTO, "foreign magic" },
        { m_version, -EPROTONOSUPPORT, "a newer version" },
        { m_hlen, -EINVAL, "header length" },
        { m_total, -EINVAL, "declared total length" },
        { m_count0, -EINVAL, "zero layers" },
        { m_count5, -EINVAL, "five layers" },
        { m_count1, -EINVAL, "count that does not match the length" },
        { m_rlen, -EINVAL, "layer record length" },
        { m_nfds_decl, -EINVAL, "declared fds differ from those received" },
        { m_nfds_given, -EINVAL, "fewer fds received than declared" },
        { m_nfds_huge, -EINVAL, "200 fds" },
        { m_flags, -EINVAL, "unknown header flags" },
        { m_crtc0, -EINVAL, "zero CRTC width" },
        { m_crtcbig, -EINVAL, "4G CRTC height" },
        { m_fbw0, -EINVAL, "zero fb width" },
        { m_fbhbig, -EINVAL, "fb height over the cap" },
        { m_np0, -EINVAL, "zero planes" },
        { m_np5, -EINVAL, "five planes" },
        { m_pitch0, -EINVAL, "zero pitch" },
        { m_offneg, -EINVAL, "offset that turns negative as EGLint" },
        { m_fdidx, -EINVAL, "plane indexes a missing fd" },
        { m_fdunused, -EINVAL, "unused plane with an fd index" },
        { m_pitchunused, -EINVAL, "unused plane with a pitch" },
        { m_srcx, -EINVAL, "SRC rect past the fb edge" },
        { m_srcw0, -EINVAL, "empty SRC rect" },
        { m_srcwrap, -EINVAL, "SRC_X that would wrap 32-bit math" },
        { m_crtcw0, -EINVAL, "empty CRTC rect" },
        { m_crtcxbig, -EINVAL, "CRTC_X far off" },
        { m_alpha, -EINVAL, "alpha over 0xffff" },
        { m_blend, -EINVAL, "unknown blend mode" },
        { m_rot0, -EINVAL, "no rotate bit" },
        { m_rot2, -EINVAL, "two rotate bits" },
        { m_rotx, -EINVAL, "unknown rotation bit" },
        { m_enc, -EINVAL, "unknown YCbCr encoding" },
        { m_range, -EINVAL, "unknown YCbCr range" },
        { m_lflags, -EINVAL, "unknown layer flags" },
        { m_order, -EINVAL, "layers out of bottom-to-top order" },
    };
    drmtap_layer_set s = sample_set();
    s.layers[1].rotation = DRMTAP_ROT_0;
    uint8_t good[DRMTAP_PKT_MAX_LEN + 8];
    int glen = drmtap_packet_encode(&s, good, DRMTAP_PKT_MAX_LEN);
    drmtap_layer_set d;
    check(drmtap_packet_decode(good, (size_t)glen, 2, &d) == 0, "the unmutated packet decodes");
    check(drmtap_packet_decode(NULL, 0, 0, &d) == -EINVAL, "NULL bytes");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t b[DRMTAP_PKT_MAX_LEN + 8];
        memcpy(b, good, sizeof(b));
        size_t len = (size_t)glen, nfds = 2;
        cases[i].f(b, &len, &nfds);
        int r = drmtap_packet_decode(b, len, nfds, &d);
        char msg[160];
        snprintf(msg, sizeof(msg), "rejects %s (%d)", cases[i].what, r);
        check(r == cases[i].want, msg);
    }
}

/* Random mutations of a valid packet. Under ASan any read outside the buffer is a
 * crash; every packet the decoder accepts must satisfy the layer invariants. */
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}

static void test_packet_fuzz(void) {
    printf("test_packet_fuzz:\n");
    drmtap_layer_set s = sample_set();
    uint8_t good[DRMTAP_PKT_MAX_LEN];
    int glen = drmtap_packet_encode(&s, good, sizeof(good));
    unsigned accepted = 0, bad_accept = 0;
    const unsigned iters = 300000;
    for (unsigned it = 0; it < iters; it++) {
        uint8_t *b = malloc(DRMTAP_PKT_MAX_LEN);   /* exact size: ASan sees overreads */
        memcpy(b, good, (size_t)glen);
        size_t len = (size_t)glen;
        unsigned flips = 1 + rnd() % 6;
        for (unsigned k = 0; k < flips; k++) {
            switch (rnd() % 4) {
            case 0: b[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8)); break;
            case 1: b[rnd() % len] = (uint8_t)rnd(); break;
            case 2: if (len >= 4) { put32(b + (rnd() % (len - 3)), rnd()); } break;
            default: len = rnd() % (size_t)glen + 1; break;
            }
        }
        size_t nfds = rnd() % 4;
        uint8_t *exact = malloc(len);
        memcpy(exact, b, len);
        drmtap_layer_set d;
        if (drmtap_packet_decode(exact, len, nfds, &d) == 0) {
            accepted++;
            int ok = d.count >= 1 && d.count <= DRMTAP_MAX_LAYERS && d.nfds == nfds &&
                     len == DRMTAP_PKT_HDR_LEN + d.count * DRMTAP_PKT_LAYER_LEN;
            for (uint32_t i = 0; i < d.count && ok; i++) {
                ok = drmtap_layer_valid(&d.layers[i], d.nfds);
            }
            if (!ok) {
                bad_accept++;
            }
        }
        free(exact);
        free(b);
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "%u mutated packets, %u accepted, none breaking an invariant",
             iters, accepted);
    check(bad_accept == 0, msg);
}

/* ===== CPU composer ===== */

static int near(int a, int b) { return a - b <= 1 && b - a <= 1; }

static void test_cpu_compose(void) {
    printf("test_cpu_compose:\n");
    enum { W = 8, H = 6 };
    uint8_t bg[W * H * 4], ov[2 * 2 * 4], cv[4], nb[4], out[W * H * 4];
    for (int i = 0; i < W * H; i++) {      /* XR24: B=10 G=20 R=30 */
        bg[i * 4] = 10; bg[i * 4 + 1] = 20; bg[i * 4 + 2] = 30; bg[i * 4 + 3] = 0x55;
    }
    for (int i = 0; i < 4; i++) {          /* AR24 premultiplied, A=128 */
        ov[i * 4] = 100; ov[i * 4 + 1] = 50; ov[i * 4 + 2] = 0; ov[i * 4 + 3] = 128;
    }
    cv[0] = 200; cv[1] = 0; cv[2] = 0; cv[3] = 64;   /* AB24: R=200, A=64 */
    nb[0] = 0; nb[1] = 255; nb[2] = 0; nb[3] = 0;    /* XR24 green, alpha ignored */
    drmtap_layer l[4];
    l[0] = mk_layer(1, W, H, 0, 0, W, H, 0);
    l[1] = mk_layer(2, 2, 2, 2, 1, 4, 4, 1);         /* 2x upscale */
    l[1].format = FMT_AR24;
    l[1].pitches[0] = 8;
    l[2] = mk_layer(3, 1, 1, -1, -1, 2, 2, 2);       /* hangs off the top-left */
    l[2].format = FMT_AB24;
    l[2].pitches[0] = 4;
    l[2].blend = DRMTAP_BLEND_COVERAGE;
    l[2].alpha = 0x8000;
    l[3] = mk_layer(4, 1, 1, 7, 5, 1, 1, 3);
    l[3].pitches[0] = 4;
    l[3].blend = DRMTAP_BLEND_NONE;
    l[3].alpha = 0x4000;
    const uint8_t *bases[4] = { bg, ov, cv, nb };
    int r = drmtap_compose_cpu(l, 4, bases, W, H, out, W * 4);
    check(r == 0, "four layers compose");
#define PX(x, y, c) out[((y) * W + (x)) * 4 + (c)]
    check(PX(7, 0, 0) == 10 && PX(7, 0, 1) == 20 && PX(7, 0, 2) == 30 && PX(7, 0, 3) == 0xff,
          "background shows where nothing covers it (X byte opaque)");
    /* premultiplied over: C + D * (1 - a) */
    double ia = 1.0 - 128.0 / 255.0;
    check(near(PX(2, 1, 0), (int)(100 + 10 * ia + 0.5)) && near(PX(5, 4, 1), (int)(50 + 20 * ia + 0.5)) &&
          near(PX(3, 2, 2), (int)(0 + 30 * ia + 0.5)),
          "pre-multiplied overlay, scaled 2x over 4x4");
    check(PX(6, 1, 0) == 10 && PX(1, 1, 0) == 10, "the scaled overlay stops at its rectangle");
    /* coverage with plane alpha: C*a + D*(1-a), a = 64/255 * 0.5 */
    double a = 64.0 / 255.0 * (32768.0 / 65535.0);
    check(near(PX(0, 0, 2), (int)(200 * a + 30 * (1 - a) + 0.5)) &&
          near(PX(0, 0, 0), (int)(10 * (1 - a) + 0.5)),
          "coverage + plane alpha, clipped at the CRTC origin");
    check(PX(1, 0, 2) == 30, "the clipped layer covers only what is on the CRTC");
    double pa = 16384.0 / 65535.0;
    check(near(PX(7, 5, 1), (int)(255 * pa + 20 * (1 - pa) + 0.5)) &&
          near(PX(7, 5, 2), (int)(30 * (1 - pa) + 0.5)),
          "blend None ignores the pixel alpha, applies the plane alpha");
#undef PX
    drmtap_layer rot = l[0];
    rot.rotation = DRMTAP_ROT_90;
    check(drmtap_compose_cpu(&rot, 1, bases, W, H, out, W * 4) == -EINVAL &&
          !drmtap_compose_cpu_supported(&rot, 1), "a rotated layer is not the CPU's job");
    drmtap_layer tiled = l[0];
    tiled.modifier = 0x0100000000000002ull;
    check(!drmtap_compose_cpu_supported(&tiled, 1), "nor a tiled one");
    drmtap_layer inval = l[0];
    inval.modifier = 0x00ffffffffffffffull;
    check(drmtap_compose_cpu_supported(&inval, 1) && !drmtap_compose_cpu_supported(&inval, 0),
          "an unknown modifier counts as linear only without EGL");
    check(drmtap_compose_cpu(l, 1, bases, W, H, out, W * 4 - 1) == -EINVAL,
          "an output stride narrower than the width is refused");
}

/* ===== drmtap_convert_packet on real DMA-BUFs ===== */

static int make_buf(int ctrl, uint32_t w, uint32_t h, uint32_t pix_bgra, int checker) {
    size_t sz = (size_t)w * h * 4;
    uint8_t *p = malloc(sz);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t v = pix_bgra;
            if (checker && x >= w / 2) {
                v = 0xff0000ffu;   /* right half: B=255 */
            } else if (checker) {
                v = 0xffff0000u;   /* left half: R=255 */
            }
            memcpy(p + ((size_t)y * w + x) * 4, &v, 4);
        }
    }
    int fd = drmtap_udmabuf_wrap(ctrl, p, sz);
    free(p);
    return fd;
}

static int convert(drmtap_ctx *ctx, const drmtap_layer_set *s, drmtap_frame_info *f,
                   uint8_t *copy, size_t cap) {
    uint8_t buf[DRMTAP_PKT_MAX_LEN];
    int len = drmtap_packet_encode(s, buf, sizeof(buf));
    if (len < 0) {
        return len;
    }
    int r = drmtap_convert_packet(ctx, buf, (size_t)len, s->fds, s->nfds, f);
    if (r == 0 && copy) {
        for (uint32_t y = 0; y < f->height; y++) {
            if ((size_t)(y + 1) * f->width * 4 <= cap) {
                memcpy(copy + (size_t)y * f->width * 4,
                       (const uint8_t *)f->data + (size_t)y * f->stride, (size_t)f->width * 4);
            }
        }
    }
    return r;
}

/* The GPU composer itself (not convert_packet, which falls back to the CPU when
 * the GPU fails and would turn a broken GPU path into a pass). */
static int gpu_compose(drmtap_ctx *ctx, const drmtap_layer_set *s, uint8_t *copy) {
    void *out = NULL;
    size_t sz = 0;
    int r = drmtap_gpu_egl_compose(ctx, s, &out, &sz);
    if (r == 0) {
        memcpy(copy, out, sz);
    }
    return r;
}

static void test_convert_packet(void) {
    printf("test_convert_packet:\n");
    int ctrl = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (ctrl < 0) {
        printf("  SKIP: /dev/udmabuf unavailable\n");
        return;
    }
    drmtap_ctx *ctx = drmtap_open_render(NULL);
    if (!ctx) {
        printf("  SKIP: no render node\n");
        close(ctrl);
        return;
    }
    enum { W = 64, H = 48 };
    drmtap_layer_set s;
    memset(&s, 0, sizeof(s));
    s.crtc_w = W; s.crtc_h = H; s.count = 4; s.lit = 4; s.nfds = 4;
    s.fds[0] = make_buf(ctrl, W, H, 0xff1e140au, 0);          /* B=10 G=20 R=30 */
    s.fds[1] = make_buf(ctrl, 16, 16, 0x80003264u, 0);         /* premult A=128 */
    s.fds[2] = make_buf(ctrl, 16, 8, 0x40c800c8u, 0);          /* A=64 */
    s.fds[3] = make_buf(ctrl, 16, 4, 0x00ff00ffu, 0);          /* alpha 0, ignored */
    if (s.fds[0] < 0 || s.fds[1] < 0 || s.fds[2] < 0 || s.fds[3] < 0) {
        printf("  SKIP: udmabuf create failed\n");
        drmtap_close(ctx);
        close(ctrl);
        return;
    }
    s.layers[0] = mk_layer(10, W, H, 0, 0, W, H, 0);
    s.layers[0].flags = DRMTAP_LAYER_FLAG_PRIMARY;
    s.layers[1] = mk_layer(11, 16, 16, 5, 7, 16, 16, 1);
    s.layers[1].format = FMT_AR24;
    s.layers[1].fd_index[0] = 1;
    s.layers[2] = mk_layer(12, 16, 8, -3, 40, 16, 8, 2);
    s.layers[2].format = FMT_AR24;
    s.layers[2].fd_index[0] = 2;
    s.layers[2].blend = DRMTAP_BLEND_COVERAGE;
    s.layers[2].alpha = 0xa000;
    s.layers[3] = mk_layer(13, 16, 4, 30, 2, 16, 4, 3);
    s.layers[3].fd_index[0] = 3;
    s.layers[3].blend = DRMTAP_BLEND_NONE;
    s.layers[3].alpha = 0x6000;

    /* Hostile input through the public entry point. */
    drmtap_frame_info f;
    uint8_t buf[DRMTAP_PKT_MAX_LEN];
    int len = drmtap_packet_encode(&s, buf, sizeof(buf));
    check(drmtap_convert_packet(ctx, buf, (size_t)len, s.fds, 3, &f) == -EINVAL,
          "fewer fds than the packet declares");
    int memfd = memfd_create("not-a-dmabuf", MFD_CLOEXEC);
    if (memfd >= 0 && ftruncate(memfd, W * H * 4) == 0) {
        int fds2[4] = { memfd, s.fds[1], s.fds[2], s.fds[3] };
        check(drmtap_convert_packet(ctx, buf, (size_t)len, fds2, 4, &f) == -EINVAL,
              "a memfd in place of a dma-buf");
    }
    if (memfd >= 0) {
        close(memfd);
    }
    /* udmabuf rounds up to a page: with pages over 4 KiB the "too small" buffers
     * below are big enough, so those two checks only run on 4 KiB pages. */
    int page4k = sysconf(_SC_PAGESIZE) <= 4096;
    if (!page4k) {
        printf("  SKIP: the undersized-buffer checks need 4 KiB pages\n");
    }
    int small = page4k ? make_buf(ctrl, 4, 4, 0, 0) : -1;
    if (small >= 0) {
        int fds3[4] = { small, s.fds[1], s.fds[2], s.fds[3] };
        check(drmtap_convert_packet(ctx, buf, (size_t)len, fds3, 4, &f) == -EINVAL,
              "a dma-buf smaller than the layer it backs");
        close(small);
    }
    buf[0] ^= 0xff;
    check(drmtap_convert_packet(ctx, buf, (size_t)len, s.fds, 4, &f) == -EPROTO,
          "a foreign packet");

    /* CPU composer through convert_packet */
    static uint8_t cpu[W * H * 4], gpu[W * H * 4];
    setenv("DRMTAP_NO_EGL", "1", 1);
    int r = convert(ctx, &s, &f, cpu, sizeof(cpu));
    check(r == 0 && f.width == W && f.height == H && f.stride == W * 4,
          "four linear layers compose on the CPU into the CRTC size");
    uint32_t rot = 0;
    check(drmtap_plane_rotation(ctx, &rot) == 0 && rot == DRMTAP_ROT_0,
          "a composed convert answers ROTATE_0");
    /* A packet that cannot be composed converts one covering layer alone (here the
     * primary); that buffer is unrotated, so the rotation answered is its own. */
    {
        drmtap_layer_set p2 = s;
        p2.count = 2; p2.lit = 2; p2.nfds = 2;
        p2.layers[0].rotation = DRMTAP_ROT_180;   /* the CPU composer refuses it */
        rot = 0;
        r = convert(ctx, &p2, &f, NULL, 0);
        int rr = drmtap_plane_rotation(ctx, &rot);
        check(r == 0 && f.width == W && f.height == H && rr == 0 && rot == DRMTAP_ROT_180,
              "a primary-alone fallback answers the primary rotation, not ROTATE_0");
    }
    /* Underlay: the primary is a 16x16 window under a covering 64x48 layer that the
     * CPU cannot compose. The fallback is the covering layer (the screen with the
     * window's hole), never the window as the screen. */
    {
        drmtap_layer_set u = s;
        u.count = 2; u.lit = 2; u.nfds = 2;
        u.layers[0] = mk_layer(10, 16, 16, 10, 10, 16, 16, 0);
        u.layers[0].flags = DRMTAP_LAYER_FLAG_PRIMARY;
        u.layers[0].fd_index[0] = 1;
        u.layers[1] = mk_layer(11, W, H, 0, 0, W, H, 1);
        u.layers[1].fd_index[0] = 0;
        u.layers[1].rotation = DRMTAP_ROT_180;   /* the CPU composer refuses it */
        rot = 0;
        r = convert(ctx, &u, &f, NULL, 0);
        int rr = drmtap_plane_rotation(ctx, &rot);
        check(r == 0 && f.width == W && f.height == H && rr == 0 && rot == DRMTAP_ROT_180,
              "an underlay fallback converts the covering layer, not the window");
        u.layers[1] = mk_layer(11, 16, 16, 30, 30, 16, 16, 1);
        u.layers[1].fd_index[0] = 1;
        u.layers[1].rotation = DRMTAP_ROT_180;
        r = convert(ctx, &u, &f, NULL, 0);
        check(r < 0 && drmtap_error(ctx) != NULL,
              "with no covering layer the fallback fails, and says why");
    }
    /* A one-layer packet: the converter reports the plane's rotation (the buffer is
     * converted unrotated), and a later convert_dmabuf forgets it again. */
    {
        drmtap_layer_set o = s;
        o.count = 1; o.lit = 1; o.nfds = 1;
        o.layers[0].rotation = DRMTAP_ROT_180;
        rot = 0;
        r = convert(ctx, &o, &f, NULL, 0);
        int rr = drmtap_plane_rotation(ctx, &rot);
        check(r == 0 && f.width == W && rr == 0 && rot == DRMTAP_ROT_180,
              "after a one-layer packet the converter answers that plane's rotation");
        r = convert(ctx, &s, &f, NULL, 0);   /* composed (CPU): ROTATE_0 */
        drmtap_dmabuf_desc d;
        memset(&d, 0, sizeof(d));
        d.dma_buf_fd = s.fds[0]; d.width = W; d.height = H; d.format = FMT_XR24;
        d.modifier = MOD_LINEAR; d.num_planes = 1; d.pitches[0] = W * 4;
        int c = drmtap_convert_dmabuf(ctx, &d, &f);
        rr = drmtap_plane_rotation(ctx, &rot);
        check(r == 0 && c == 0 && rr == -ENOTSUP,
              "convert_dmabuf after a composed convert: the render ctx answers -ENOTSUP again");
    }
    unsetenv("DRMTAP_NO_EGL");

    /* the same layers on the GPU */
    if (!drmtap_gpu_egl_available(ctx)) {
        printf("  SKIP: no EGL on this render node\n");
    } else {
        r = gpu_compose(ctx, &s, gpu);
        check(r == 0, "the GPU composes the four udmabuf layers");
        int maxd = 0;
        for (size_t i = 0; i < sizeof(cpu); i++) {
            if ((i & 3) == 3) {
                continue;   /* X byte */
            }
            int d = cpu[i] > gpu[i] ? cpu[i] - gpu[i] : gpu[i] - cpu[i];
            if (d > maxd) {
                maxd = d;
            }
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "GPU and CPU composition agree (max channel diff %d)", maxd);
        check(maxd <= 1, msg);

        /* scaling: a uniform layer stretched 3x lands on exactly its rectangle */
        drmtap_layer_set t = s;
        t.count = 2;
        t.layers[1].crtc_x = 10; t.layers[1].crtc_y = 4;
        t.layers[1].crtc_w = 48; t.layers[1].crtc_h = 33;
        t.layers[1].format = FMT_XR24;
        r = gpu_compose(ctx, &t, gpu);
        const uint8_t *g = gpu;
        check(r == 0 && g[(4 * W + 10) * 4] == 100 && g[(36 * W + 57) * 4] == 100 &&
              g[(3 * W + 10) * 4] == 10 && g[(4 * W + 9) * 4] == 10 &&
              g[(37 * W + 57) * 4] == 10 && g[(36 * W + 58) * 4] == 10,
              "a scaled layer covers exactly its CRTC rectangle on the GPU");

        /* rotation on the GPU: 16x8 fb, left half red, right half blue, rotate-90
         * (counter-clockwise) into an 8x16 rectangle: blue on top, red below */
        int rb = make_buf(ctrl, 16, 8, 0, 1);
        if (rb >= 0) {
            drmtap_layer_set u;
            memset(&u, 0, sizeof(u));
            u.crtc_w = 32; u.crtc_h = 32; u.count = 2; u.lit = 2; u.nfds = 2;
            u.fds[0] = s.fds[0];
            u.fds[1] = rb;
            u.layers[0] = mk_layer(10, W, H, 0, 0, 32, 32, 0);
            u.layers[0].src_w = 32u << 16; u.layers[0].src_h = 32u << 16;
            u.layers[1] = mk_layer(11, 16, 8, 4, 2, 8, 16, 1);
            u.layers[1].fd_index[0] = 1;
            u.layers[1].rotation = DRMTAP_ROT_90;
            r = gpu_compose(ctx, &u, gpu);
            const uint8_t *top = gpu + ((size_t)3 * 32 + 5) * 4;     /* (5,3) */
            const uint8_t *bot = gpu + ((size_t)16 * 32 + 10) * 4;   /* (10,16) */
            check(r == 0 && top[0] == 255 && top[2] == 0 && bot[2] == 255 && bot[0] == 0,
                  "rotate-90 puts the right half of the source on top");
            u.layers[1].rotation = DRMTAP_ROT_270;
            r = gpu_compose(ctx, &u, gpu);
            check(r == 0 && top[2] == 255 && top[0] == 0 && bot[0] == 255 && bot[2] == 0,
                  "rotate-270 puts it at the bottom");
            close(rb);
        }

        /* One NV12 layer, rotate-180, placed at +16+8 of a 64x48 CRTC: converted as
         * its buffer (32x16, unrotated: white left half, black right half) and the
         * rotation is the converter's answer. Twice: luma and chroma in separate
         * fds, then both in one fd (the usual NV12 layout, chroma at YW*YH). */
        for (int onefd = 0; onefd < 2; onefd++) {
            enum { YW = 32, YH = 16 };
            uint8_t *yb = malloc(4096), *cb = malloc(4096);
            memset(yb, 0, 4096);
            memset(cb, 128, 4096);
            for (int y = 0; y < YH; y++) {
                for (int x = 0; x < YW; x++) {
                    yb[y * YW + x] = x < YW / 2 ? 235 : 16;
                }
            }
            if (onefd) {
                memset(yb + YW * YH, 128, YW * YH / 2);
            }
            int yfd = drmtap_udmabuf_wrap(ctrl, yb, 4096);
            int cfd = onefd ? -1 : drmtap_udmabuf_wrap(ctrl, cb, 4096);
            free(yb);
            free(cb);
            if (yfd >= 0 && (onefd || cfd >= 0)) {
                drmtap_layer_set n;
                memset(&n, 0, sizeof(n));
                n.crtc_w = W; n.crtc_h = H; n.count = 1; n.lit = 1; n.nfds = onefd ? 1 : 2;
                n.fds[0] = yfd; n.fds[1] = cfd;
                n.layers[0] = mk_layer(10, YW, YH, 16, 8, YW, YH, 0);
                n.layers[0].format = 0x3231564eu;   /* NV12 */
                n.layers[0].num_planes = 2;
                n.layers[0].pitches[0] = YW;
                n.layers[0].pitches[1] = YW;
                n.layers[0].fd_index[1] = onefd ? 0 : 1;
                n.layers[0].offsets[1] = onefd ? YW * YH : 0;
                n.layers[0].rotation = DRMTAP_ROT_180;
                n.layers[0].flags = DRMTAP_LAYER_FLAG_PRIMARY;
                static uint8_t one[YW * YH * 4];
                uint32_t rot = 0;
                r = convert(ctx, &n, &f, one, sizeof(one));
                int rr = drmtap_plane_rotation(ctx, &rot);
                check(r == 0 && f.width == YW && f.height == YH && rr == 0 &&
                      rot == DRMTAP_ROT_180 && one[(8 * YW + 2) * 4 + 1] > 200 &&
                      one[(8 * YW + 29) * 4 + 1] < 40,
                      onefd ? "a one-layer NV12 packet in one fd converts as its buffer, unrotated"
                            : "a one-layer packet in two fds converts as its buffer, unrotated");
            }
            if (yfd >= 0) close(yfd);
            if (cfd >= 0) close(cfd);
        }
        /* Import-cache pinning: a later layer must never release the texture an
         * earlier layer of the same frame already picked. Fill the 12 slots, then
         * a frame whose bottom layer hits and whose top layer misses (eviction),
         * and one whose top layer reuses the bottom fb_id with another geometry. */
        int red = make_buf(ctrl, W, H, 0xffff0000u, 0);
        int blue = make_buf(ctrl, 16, 16, 0xff0000ffu, 0);
        if (red >= 0 && blue >= 0) {
            drmtap_layer_set e;
            for (int k = 0; k < 6; k++) {
                memset(&e, 0, sizeof(e));
                e.crtc_w = W; e.crtc_h = H; e.count = 2; e.lit = 2; e.nfds = 1;
                e.fds[0] = red;
                e.layers[0] = mk_layer(10, W, H, 0, 0, W, H, 0);
                e.layers[0].fb_id = 1000 + 2 * k;
                e.layers[1] = mk_layer(11, W, H, 0, 0, W, H, 1);
                e.layers[1].fb_id = 1001 + 2 * k;
                gpu_compose(ctx, &e, gpu);
            }
            for (int mode = 0; mode < 2; mode++) {
                memset(&e, 0, sizeof(e));
                e.crtc_w = W; e.crtc_h = H; e.count = 2; e.lit = 2; e.nfds = 2;
                e.fds[0] = red;
                e.fds[1] = blue;
                e.layers[0] = mk_layer(10, W, H, 0, 0, W, H, 0);
                e.layers[0].fb_id = 1000;
                e.layers[1] = mk_layer(11, 16, 16, 0, 0, 16, 16, 1);
                e.layers[1].fd_index[0] = 1;
                e.layers[1].fb_id = mode == 0 ? 4242 : 1000;
                r = gpu_compose(ctx, &e, gpu);
                const uint8_t *px = gpu + ((size_t)40 * W + 40) * 4;
                const uint8_t *top = gpu + ((size_t)4 * W + 4) * 4;
                check(r == 0 && px[2] == 255 && px[0] == 0 && top[0] == 255,
                      mode == 0 ? "a cache eviction does not black out a layer of the same frame"
                                : "the same fb_id with another layout in one frame keeps both");
            }
            /* more layers than free slots: run several frames, all still exact */
            int bad = 0;
            for (int k = 0; k < 40; k++) {
                memset(&e, 0, sizeof(e));
                e.crtc_w = W; e.crtc_h = H; e.count = 4; e.lit = 4; e.nfds = 2;
                e.fds[0] = red;
                e.fds[1] = blue;
                e.layers[0] = mk_layer(10, W, H, 0, 0, W, H, 0);
                e.layers[0].fb_id = 2000 + (uint32_t)k * 4;
                for (int j = 1; j < 4; j++) {
                    e.layers[j] = mk_layer(10 + (uint32_t)j, 16, 16, 16 * j, 0, 16, 16, (uint32_t)j);
                    e.layers[j].fd_index[0] = 1;
                    e.layers[j].fb_id = 2000 + (uint32_t)k * 4 + (uint32_t)j;
                }
                if (gpu_compose(ctx, &e, gpu) != 0 || gpu[((size_t)40 * W + 40) * 4 + 2] != 255 ||
                    gpu[((size_t)4 * W + 20) * 4] != 255) {
                    bad++;
                }
            }
            check(bad == 0, "40 frames of 4 fresh layers, cache always full: no layer lost");
        }
        if (red >= 0) {
            close(red);
        }
        if (blue >= 0) {
            close(blue);
        }

        /* a composed convert is in CRTC orientation */
        r = convert(ctx, &s, &f, NULL, 0);
        uint32_t rot = 0;
        check(r == 0 && drmtap_plane_rotation(ctx, &rot) == 0 && rot == DRMTAP_ROT_0,
              "after a composed convert, plane_rotation answers ROTATE_0");
    }

    /* a chroma plane in an fd of its own is bounded by its whole extent */
    {
        int y = make_buf(ctrl, 16, 16, 0, 0);       /* 1024 bytes: plenty for 64x16 luma */
        int uv_small = make_buf(ctrl, 1, 1, 0, 0);  /* one page */
        int uv_big = make_buf(ctrl, 64, 64, 0, 0);  /* 16 KiB */
        if (y >= 0 && uv_small >= 0 && uv_big >= 0) {
            drmtap_layer_set n;
            memset(&n, 0, sizeof(n));
            n.crtc_w = W; n.crtc_h = H; n.count = 1; n.lit = 1; n.nfds = 2;
            n.fds[0] = y;
            n.fds[1] = uv_small;
            n.layers[0] = mk_layer(10, 64, 16, 0, 0, 64, 16, 0);
            n.layers[0].format = 0x3231564eu;   /* NV12 */
            n.layers[0].num_planes = 2;
            n.layers[0].pitches[0] = 64;
            n.layers[0].pitches[1] = 512;       /* 8 rows x 512 = 4096: fits one page */
            n.layers[0].fd_index[1] = 1;
            n.layers[0].src_w = 64u << 16; n.layers[0].src_h = 16u << 16;
            check(convert(ctx, &n, &f, NULL, 0) != -EINVAL,
                  "NV12 chroma that fits its own fd passes validation");
            n.layers[0].pitches[1] = 1024;      /* 8 rows x 1024 = 8192 > 4096 */
            if (page4k) {
                check(convert(ctx, &n, &f, NULL, 0) == -EINVAL,
                      "NV12 chroma larger than its own fd is refused");
            }
            n.fds[1] = uv_big;
            check(convert(ctx, &n, &f, NULL, 0) != -EINVAL,
                  "and accepted when its fd is big enough");
            n.layers[0].pitches[1] = 0;
            check(convert(ctx, &n, &f, NULL, 0) == -EINVAL, "NV12 chroma stride 0 is refused");
            n.layers[0].pitches[1] = 1;
            check(convert(ctx, &n, &f, NULL, 0) == -EINVAL,
                  "NV12 chroma stride narrower than the row (1 < 64) is refused");
            n.layers[0].pitches[1] = 64;
            check(convert(ctx, &n, &f, NULL, 0) != -EINVAL, "NV12 chroma stride 64 is accepted");
            /* a hostile fourcc never puts control bytes into the message */
            n.layers[0].format = 0x0d1b4141u;   /* 'A' 'A' ESC CR */
            n.layers[0].num_planes = 1;
            n.layers[0].fd_index[1] = DRMTAP_PKT_NO_FD;
            n.layers[0].pitches[1] = 0;
            n.layers[0].pitches[0] = 8;          /* < 64 * 4: refused, with the fourcc */
            n.nfds = 1;
            int rc = convert(ctx, &n, &f, NULL, 0);
            const char *e = drmtap_error(ctx);
            int clean = e != NULL && strstr(e, "0x0d1b4141") != NULL;
            for (const char *c = e; c && *c; c++) {
                clean = clean && (unsigned char)*c >= 0x20 && (unsigned char)*c < 0x7f;
            }
            check(rc == -EINVAL && clean, "a hostile fourcc is printed as hex, no control bytes");
        }
        if (y >= 0) close(y);
        if (uv_small >= 0) close(uv_small);
        if (uv_big >= 0) close(uv_big);
    }
    for (int i = 0; i < 4; i++) {
        close(s.fds[i]);
    }
    drmtap_close(ctx);
    close(ctrl);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running multi-plane layer tests...\n");
    test_sort_and_cover();
    test_quad();
    test_key();
    test_underlay_timing();
    test_packet_init();
    test_stand_in();
    test_cover_rect();
    test_packet_roundtrip();
    test_packet_malformed();
    test_packet_fuzz();
    test_cpu_compose();
    test_convert_packet();
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("All layer tests passed!\n");
    return 0;
}
