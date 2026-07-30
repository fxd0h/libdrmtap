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
 * No hardware needed. The case this covers was found on an Apple T2 MacBook,
 * whose Touch Bar card (appletbdrm) drives a 60x2170 mode from a 64x2170
 * framebuffer with a 256-byte pitch. Reporting the framebuffer width made a
 * consumer see every frame as disagreeing with the display geometry it had
 * enumerated. That is one laptop in the world, so the decision itself is a pure
 * function and is asserted here instead of only on that machine.
 */

#include <stdio.h>
#include <stdlib.h>

#include "drmtap.h"
#include "drmtap_internal.h"

/* The layout argument, named so the cases read as what they are. Only a linear
 * (or unknown-and-treated-as-linear) scanout may be narrowed; see the TILED case. */
#define LINEAR 1
#define TILED  0

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

/* The measured Touch Bar numbers: mode 60 wide, framebuffer 64 wide, viewport at
 * the framebuffer origin. The four extra columns are pitch padding and were never
 * scanned out, so the reported width must be the mode's. */
static void test_padded_fb_is_narrowed_to_the_mode(void) {
    drmtap_scanout_why why = DRMTAP_SCANOUT_AS_IS;
    TEST_ASSERT(drmtap_scanout_width_of(64, 1, 60, 0, 0, LINEAR, &why) == 60);
    TEST_ASSERT(why == DRMTAP_SCANOUT_NARROWED);
}

/* The common case, measured on the same laptop's amdgpu panel (2880x1800 mode
 * from a 2880x1800 framebuffer): nothing to narrow, and no reason logged. */
static void test_unpadded_fb_is_untouched(void) {
    drmtap_scanout_why why = DRMTAP_SCANOUT_NARROWED;
    TEST_ASSERT(drmtap_scanout_width_of(2880, 1, 2880, 0, 0, LINEAR, &why) == 2880);
    TEST_ASSERT(why == DRMTAP_SCANOUT_AS_IS);
}

/* A framebuffer NARROWER than the mode is a CRTC scaling a smaller buffer up to
 * its mode. That is a different image, not padding, so it must never be widened
 * -- the function only ever shrinks. The consumer decides what to do with it. */
static void test_upscaling_crtc_is_never_widened(void) {
    drmtap_scanout_why why = DRMTAP_SCANOUT_NARROWED;
    TEST_ASSERT(drmtap_scanout_width_of(1280, 1, 3840, 0, 0, LINEAR, &why) == 1280);
    TEST_ASSERT(why == DRMTAP_SCANOUT_AS_IS);
}

/* Several heads scanning out of one big framebuffer: the visible region does not
 * start at the framebuffer origin, so narrowing the width alone would report the
 * WRONG pixels (the left head's) for this CRTC. drmtap_dmabuf_desc has a frozen
 * layout with nowhere to put a crop origin, so the whole framebuffer is reported
 * and the caller is told why. Untested on hardware -- we have no such setup. */
static void test_offset_viewport_is_left_whole(void) {
    drmtap_scanout_why why = DRMTAP_SCANOUT_AS_IS;
    TEST_ASSERT(drmtap_scanout_width_of(3840, 1, 1920, 1920, 0, LINEAR, &why) == 3840);
    TEST_ASSERT(why == DRMTAP_SCANOUT_OFFSET_UNSUPPORTED);
    /* A vertical stack is the same situation. */
    TEST_ASSERT(drmtap_scanout_width_of(1920, 1, 1080, 0, 1080, LINEAR, &why) == 1920);
    TEST_ASSERT(why == DRMTAP_SCANOUT_OFFSET_UNSUPPORTED);
}

/* A PADDED TILED scanout must NOT be narrowed, even though it is exactly the shape
 * the linear case narrows. The CPU deswizzle derives its tile grid from the width
 * and uses it to address the SOURCE (tiles_x = ceil(width/tile_w), then
 * src_off = (tile_row * tiles_x + tx) * tile_size), so a width short by a tile or
 * more mis-addresses every tile row after the first and silently mangles the image.
 * The visible width and the width the tiling was laid out for are different things.
 * No padded tiled scanout has been observed on any hardware here, so this is the
 * conservative branch: report the framebuffer, exactly as before this feature. */
static void test_padded_tiled_fb_is_not_narrowed(void) {
    drmtap_scanout_why why = DRMTAP_SCANOUT_AS_IS;
    TEST_ASSERT(drmtap_scanout_width_of(64, 1, 60, 0, 0, TILED, &why) == 64);
    TEST_ASSERT(why == DRMTAP_SCANOUT_TILED_NOT_NARROWED);
    /* A tiled scanout that needs no narrowing is still just as-is. */
    TEST_ASSERT(drmtap_scanout_width_of(2880, 1, 2880, 0, 0, TILED, &why) == 2880);
    TEST_ASSERT(why == DRMTAP_SCANOUT_AS_IS);
}

/* Without a valid mode there is no scanned-out width to clamp to, and a zero
 * hdisplay must never produce a zero-width frame (it would divide by zero
 * downstream and reads as a successful capture of nothing). */
static void test_missing_mode_falls_back_to_the_fb(void) {
    drmtap_scanout_why why = DRMTAP_SCANOUT_NARROWED;
    TEST_ASSERT(drmtap_scanout_width_of(1920, 0, 1080, 0, 0, LINEAR, &why) == 1920);
    TEST_ASSERT(why == DRMTAP_SCANOUT_AS_IS);
    TEST_ASSERT(drmtap_scanout_width_of(1920, 1, 0, 0, 0, LINEAR, &why) == 1920);
    TEST_ASSERT(why == DRMTAP_SCANOUT_AS_IS);
}

/* The reason pointer is optional for callers that only want the number. */
static void test_why_may_be_null(void) {
    TEST_ASSERT(drmtap_scanout_width_of(64, 1, 60, 0, 0, LINEAR, NULL) == 60);
}

int main(void) {
    printf("== scanout width ==\n");
    test_padded_fb_is_narrowed_to_the_mode();
    test_unpadded_fb_is_untouched();
    test_upscaling_crtc_is_never_widened();
    test_offset_viewport_is_left_whole();
    test_padded_tiled_fb_is_not_narrowed();
    test_missing_mode_falls_back_to_the_fb();
    test_why_may_be_null();
    printf("all scanout width tests passed\n");
    return 0;
}
