/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file test_scanout.c
 * @brief Unit test — the scanned-out width of a padded scanout framebuffer
 *
 * No hardware needed. The case this covers was found on an Apple T2 MacBook, whose
 * Touch Bar card (appletbdrm) drives a 60x2170 mode from a 64x2170 framebuffer with
 * a 256-byte pitch, its primary plane reading SRC 60x2170 onto a 60x2170 CRTC rect.
 * Reporting the framebuffer width made a consumer see every frame as disagreeing
 * with the display geometry it had enumerated. That is one laptop in the world, so
 * the decision is a pure function and is asserted here instead of only on it.
 *
 * The cases that must NOT narrow matter more than the one that must, because each
 * of them silently returns a WRONG image rather than failing.
 */

#include <stdio.h>
#include <stdlib.h>

#include "drmtap.h"
#include "drmtap_internal.h"

/* Layout argument, named so the cases read as what they are. */
#define LINEAR 1
#define TILED  0

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

/* A plane showing its whole source 1:1 at the framebuffer origin. */
static drmtap_plane_rect rect_1to1(uint32_t w, uint32_t h) {
    drmtap_plane_rect r = {0};
    r.valid = 1;
    r.src_w = w; r.src_h = h;
    r.crtc_w = w; r.crtc_h = h;
    return r;
}

/* The measured Touch Bar numbers: framebuffer 64 wide, plane reads 60 columns 1:1.
 * The four extra columns are pitch padding and are never scanned out. */
static void test_padded_fb_is_narrowed_to_what_the_plane_reads(void) {
    drmtap_scanout_why why = DRMTAP_SCANOUT_AS_IS;
    drmtap_plane_rect r = rect_1to1(60, 2170);
    TEST_ASSERT(drmtap_scanout_width_of(64, &r, LINEAR, &why) == 60);
    TEST_ASSERT(why == DRMTAP_SCANOUT_NARROWED);
}

/* The common case, measured on the same laptop's amdgpu panel (2880x1800 out of a
 * 2880x1800 framebuffer): nothing to narrow, and no reason reported. */
static void test_unpadded_fb_is_untouched(void) {
    drmtap_scanout_why why = DRMTAP_SCANOUT_NARROWED;
    drmtap_plane_rect r = rect_1to1(2880, 1800);
    TEST_ASSERT(drmtap_scanout_width_of(2880, &r, LINEAR, &why) == 2880);
    TEST_ASSERT(why == DRMTAP_SCANOUT_AS_IS);
}

/* A plane that genuinely DOWNSCALES. The whole framebuffer is being displayed, just
 * smaller, so narrowing would hand back the left part of the image and call it the
 * screen. This looks identical to pitch padding if you only compare the framebuffer
 * width against the mode, which is why the plane rect is consulted at all. Note the
 * pitch cannot substitute: a 3840-wide framebuffer has pitch == width*4 exactly, the
 * same as the padded 64-wide one does. */
static void test_downscaling_plane_is_not_narrowed(void) {
    drmtap_scanout_why why = DRMTAP_SCANOUT_AS_IS;
    drmtap_plane_rect r = {0};
    r.valid = 1;
    r.src_w = 3840; r.src_h = 2160;    /* reads the whole 3840-wide fb ... */
    r.crtc_w = 1920; r.crtc_h = 1080;  /* ... onto a 1920 mode */
    TEST_ASSERT(drmtap_scanout_width_of(3840, &r, LINEAR, &why) == 3840);
    TEST_ASSERT(why == DRMTAP_SCANOUT_SCALING_NOT_NARROWED);
    /* Upscaling is the same refusal: the source is not padded, it is stretched. */
    drmtap_plane_rect u = {0};
    u.valid = 1;
    u.src_w = 1280; u.src_h = 720;
    u.crtc_w = 3840; u.crtc_h = 2160;
    TEST_ASSERT(drmtap_scanout_width_of(1280, &u, LINEAR, &why) == 1280);
    TEST_ASSERT(why == DRMTAP_SCANOUT_SCALING_NOT_NARROWED);
}

/* A framebuffer NARROWER than what the plane reads is not something to widen: this
 * function only ever shrinks. */
static void test_never_widens(void) {
    drmtap_scanout_why why = DRMTAP_SCANOUT_NARROWED;
    drmtap_plane_rect r = rect_1to1(3840, 2160);
    TEST_ASSERT(drmtap_scanout_width_of(1280, &r, LINEAR, &why) == 1280);
    TEST_ASSERT(why == DRMTAP_SCANOUT_AS_IS);
}

/* Several heads scanning out of one big framebuffer: the plane reads from a non-zero
 * origin, so narrowing the width alone would report the WRONG columns (the left
 * head's) for this CRTC. drmtap_dmabuf_desc has a frozen layout with nowhere to put a
 * crop origin, so the whole framebuffer is reported and the caller is told why.
 * Untested on hardware - we have no such setup. */
static void test_offset_source_is_left_whole(void) {
    drmtap_scanout_why why = DRMTAP_SCANOUT_AS_IS;
    drmtap_plane_rect r = rect_1to1(1920, 1080);
    r.src_x = 1920;
    TEST_ASSERT(drmtap_scanout_width_of(3840, &r, LINEAR, &why) == 3840);
    TEST_ASSERT(why == DRMTAP_SCANOUT_OFFSET_UNSUPPORTED);
    /* A vertical stack is the same situation. */
    drmtap_plane_rect v = rect_1to1(1920, 1080);
    v.src_y = 1080;
    TEST_ASSERT(drmtap_scanout_width_of(1920, &v, LINEAR, &why) == 1920);
    TEST_ASSERT(why == DRMTAP_SCANOUT_OFFSET_UNSUPPORTED);
}

/* A padded TILED scanout must NOT be narrowed either. The CPU deswizzle derives its
 * tile grid from the width and uses it to address the SOURCE (tiles_x =
 * ceil(width/tile_w), then src_off = (tile_row * tiles_x + tx) * tile_size), so a
 * width short by a tile or more mis-addresses every tile row after the first and
 * silently mangles the image. The visible width and the width the tiling was laid out
 * for are different things. No padded tiled scanout has been observed on any hardware
 * here, so this keeps the behaviour that predates the narrowing. */
static void test_padded_tiled_fb_is_not_narrowed(void) {
    drmtap_scanout_why why = DRMTAP_SCANOUT_AS_IS;
    drmtap_plane_rect r = rect_1to1(60, 2170);
    TEST_ASSERT(drmtap_scanout_width_of(64, &r, TILED, &why) == 64);
    TEST_ASSERT(why == DRMTAP_SCANOUT_TILED_NOT_NARROWED);
    /* A tiled scanout with nothing to narrow reports no reason at all. */
    drmtap_plane_rect ok = rect_1to1(2880, 1800);
    TEST_ASSERT(drmtap_scanout_width_of(2880, &ok, TILED, &why) == 2880);
    TEST_ASSERT(why == DRMTAP_SCANOUT_AS_IS);
}

/* An unreadable plane rect is NOT the same as "no scaling", and must not be treated
 * as one: without it, pitch padding and a downscaling plane are indistinguishable, so
 * the safe answer is the framebuffer. The properties are atomic-only and are hidden
 * from a client that has not asked for DRM_CLIENT_CAP_ATOMIC, which is exactly how
 * this can happen in the field. */
static void test_unreadable_plane_rect_does_not_narrow(void) {
    drmtap_scanout_why why = DRMTAP_SCANOUT_AS_IS;
    drmtap_plane_rect r = {0};   /* valid == 0 */
    TEST_ASSERT(drmtap_scanout_width_of(64, &r, LINEAR, &why) == 64);
    TEST_ASSERT(why == DRMTAP_SCANOUT_NO_PLANE_RECT);
    /* A NULL rect is the same situation and must not dereference. */
    why = DRMTAP_SCANOUT_AS_IS;
    TEST_ASSERT(drmtap_scanout_width_of(64, NULL, LINEAR, &why) == 64);
    TEST_ASSERT(why == DRMTAP_SCANOUT_NO_PLANE_RECT);
    /* Tiled with no rect: also unchanged, and no tiled reason to report since there
     * is nothing known to narrow. */
    why = DRMTAP_SCANOUT_NARROWED;
    TEST_ASSERT(drmtap_scanout_width_of(64, NULL, TILED, &why) == 64);
    TEST_ASSERT(why == DRMTAP_SCANOUT_AS_IS);
}

/* A zero source width must never produce a zero-width frame: it would divide by zero
 * downstream and read as a successful capture of nothing. */
static void test_zero_source_width_never_yields_zero(void) {
    drmtap_scanout_why why = DRMTAP_SCANOUT_NARROWED;
    drmtap_plane_rect r = {0};
    r.valid = 1;                 /* readable, but reporting nothing useful */
    TEST_ASSERT(drmtap_scanout_width_of(1920, &r, LINEAR, &why) == 1920);
    TEST_ASSERT(why == DRMTAP_SCANOUT_AS_IS);
}

/* The reason pointer is optional for callers that only want the number. */
static void test_why_may_be_null(void) {
    drmtap_plane_rect r = rect_1to1(60, 2170);
    TEST_ASSERT(drmtap_scanout_width_of(64, &r, LINEAR, NULL) == 60);
}

int main(void) {
    printf("== scanout width ==\n");
    test_padded_fb_is_narrowed_to_what_the_plane_reads();
    test_unpadded_fb_is_untouched();
    test_downscaling_plane_is_not_narrowed();
    test_never_widens();
    test_offset_source_is_left_whole();
    test_padded_tiled_fb_is_not_narrowed();
    test_unreadable_plane_rect_does_not_narrow();
    test_zero_source_width_never_yields_zero();
    test_why_may_be_null();
    printf("all scanout width tests passed\n");
    return 0;
}
