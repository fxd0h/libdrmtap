/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file test_outbuf.c
 * @brief Unit test — caller-supplied output buffer, and its bounds
 *
 * No hardware needed. drmtap_set_output_buffer() hands conversion code a pointer
 * and a length that come from OUTSIDE the library, so the bound is the whole
 * safety story: a frame that does not fit must be refused, never written short and
 * never written past the end. These tests pin the refusal, the argument checks and
 * the dimension bound that keeps width*height*4 from wrapping size_t.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drmtap.h"
#include "drmtap_internal.h"

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

/* drmtap_ensure_out is the single place every converted-pixel producer resolves
 * its destination, so testing it tests all of them. A bare context is enough: it
 * touches only user_out/user_out_len and the owned buffer. */
static drmtap_ctx *bare_ctx(void) {
    drmtap_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT(ctx != NULL);
    return ctx;
}

static void free_ctx(drmtap_ctx *ctx) {
    free(ctx->deswizzle_buf);
    free(ctx);
}

/* With no output buffer set, the destination is the library's own grow-once
 * buffer -- the behaviour every existing caller already depends on. */
static void test_default_uses_the_library_buffer(void) {
    drmtap_ctx *ctx = bare_ctx();
    void *out = NULL;
    TEST_ASSERT(drmtap_ensure_out(ctx, 4096, &out) == 0);
    TEST_ASSERT(out == ctx->deswizzle_buf);
    TEST_ASSERT(out != NULL);
    free_ctx(ctx);
}

/* A buffer that fits is handed back as the destination, with no allocation. */
static void test_a_fitting_buffer_is_used(void) {
    drmtap_ctx *ctx = bare_ctx();
    unsigned char mine[4096];
    TEST_ASSERT(drmtap_set_output_buffer(ctx, mine, sizeof mine) == 0);
    void *out = NULL;
    TEST_ASSERT(drmtap_ensure_out(ctx, sizeof mine, &out) == 0);
    TEST_ASSERT(out == mine);
    TEST_ASSERT(ctx->deswizzle_buf == NULL);  /* nothing allocated behind it */
    free_ctx(ctx);
}

/* THE bound. One byte over must be refused, and the buffer must be left alone --
 * a short frame is indistinguishable from a good one to the caller, and writing
 * past the end is the whole class of bug this API could otherwise introduce. */
static void test_one_byte_too_small_is_refused_untouched(void) {
    drmtap_ctx *ctx = bare_ctx();
    unsigned char mine[64];
    memset(mine, 0xAB, sizeof mine);
    TEST_ASSERT(drmtap_set_output_buffer(ctx, mine, sizeof mine) == 0);
    /* Pre-set with a valid address, not an integer cast, so the assertion below is
     * about ensure_out leaving *out alone on refusal and not about a bogus pointer. */
    void *sentinel = &ctx;
    void *out = sentinel;
    TEST_ASSERT(drmtap_ensure_out(ctx, sizeof mine + 1, &out) == -ENOSPC);
    TEST_ASSERT(out == sentinel);   /* untouched on refusal */
    for (size_t i = 0; i < sizeof mine; i++) {
        TEST_ASSERT(mine[i] == 0xAB);
    }
    /* And it says how many bytes were needed, so the caller can resize instead of
     * guessing. */
    TEST_ASSERT(strstr(drmtap_error(ctx), "65") != NULL);
    free_ctx(ctx);
}

/* Exactly the right size is not off by one in the other direction either. */
static void test_exact_fit_is_accepted(void) {
    drmtap_ctx *ctx = bare_ctx();
    unsigned char mine[100];
    TEST_ASSERT(drmtap_set_output_buffer(ctx, mine, sizeof mine) == 0);
    void *out = NULL;
    TEST_ASSERT(drmtap_ensure_out(ctx, sizeof mine, &out) == 0);
    TEST_ASSERT(out == mine);
    free_ctx(ctx);
}

/* Clearing returns to the library buffer, so a caller can hand memory back before
 * unmapping it. */
static void test_clearing_returns_to_the_library_buffer(void) {
    drmtap_ctx *ctx = bare_ctx();
    unsigned char mine[4096];
    TEST_ASSERT(drmtap_set_output_buffer(ctx, mine, sizeof mine) == 0);
    TEST_ASSERT(drmtap_set_output_buffer(ctx, NULL, 0) == 0);
    void *out = NULL;
    TEST_ASSERT(drmtap_ensure_out(ctx, 4096, &out) == 0);
    TEST_ASSERT(out == ctx->deswizzle_buf);
    free_ctx(ctx);
}

/* Argument checks. A non-NULL buffer with length 0 is a caller bug, not a request
 * to clear: accepting it would set a zero-length destination that every frame then
 * fails against, which reads as "capture is broken" instead of "fix the call". */
static void test_argument_checks(void) {
    drmtap_ctx *ctx = bare_ctx();
    unsigned char mine[16];
    TEST_ASSERT(drmtap_set_output_buffer(NULL, mine, sizeof mine) == -EINVAL);
    TEST_ASSERT(drmtap_set_output_buffer(ctx, mine, 0) == -EINVAL);
    TEST_ASSERT(ctx->user_out == NULL);
    free_ctx(ctx);
}

/* The frame cap applies to the caller's buffer too: owning a huge mapping must not
 * unlock a frame bigger than this library handles. */
static void test_the_frame_cap_still_applies(void) {
    drmtap_ctx *ctx = bare_ctx();
    /* A REAL address with a length that lies about how much is behind it: ensure_out
     * must refuse on the size alone and never dereference, so a valid pointer is
     * enough and an integer cast would only muddy what is being asserted. */
    unsigned char small[16];
    ctx->user_out = small;
    ctx->user_out_len = DRMTAP_MAX_FB_BYTES * 4;
    void *out = NULL;
    TEST_ASSERT(drmtap_ensure_out(ctx, DRMTAP_MAX_FB_BYTES + 1, &out) == -EFBIG);
    ctx->user_out = NULL;
    free_ctx(ctx);
}

/* A zero-byte frame is nonsense on either destination. */
static void test_zero_size_is_rejected(void) {
    drmtap_ctx *ctx = bare_ctx();
    void *out = NULL;
    TEST_ASSERT(drmtap_ensure_out(ctx, 0, &out) == -EINVAL);
    free_ctx(ctx);
}

/* The dimension bound that keeps width*height*4 from wrapping size_t. Without it,
 * a hostile or corrupt geometry could size a destination small and then have the
 * detile write the real, much larger frame into it. */
static void test_dimension_bound_is_wrap_proof(void) {
    TEST_ASSERT(drmtap_validate_fb_dims(1920, 1080) == 0);
    TEST_ASSERT(drmtap_validate_fb_dims(7680, 4320) == 0);      /* 8K, the documented cap */
    TEST_ASSERT(drmtap_validate_fb_dims(0, 1080) == -EINVAL);
    TEST_ASSERT(drmtap_validate_fb_dims(1920, 0) == -EINVAL);
    /* Over the per-dimension ceiling: refused before anything multiplies it. */
    TEST_ASSERT(drmtap_validate_fb_dims(DRMTAP_MAX_DIM + 1, 1) == -EFBIG);
    TEST_ASSERT(drmtap_validate_fb_dims(1, DRMTAP_MAX_DIM + 1) == -EFBIG);
    /* The exact values that would wrap a 64-bit width*height*4. */
    TEST_ASSERT(drmtap_validate_fb_dims(0xFFFFFFFFu, 0xFFFFFFFFu) == -EFBIG);
    TEST_ASSERT(drmtap_validate_fb_dims(0x80000000u, 0x80000000u) == -EFBIG);
    /* In range per dimension, but too many pixels together. */
    TEST_ASSERT(drmtap_validate_fb_dims(DRMTAP_MAX_DIM, DRMTAP_MAX_DIM) == -EFBIG);
}

int main(void) {
    printf("== output buffer ==\n");
    test_default_uses_the_library_buffer();
    test_a_fitting_buffer_is_used();
    test_one_byte_too_small_is_refused_untouched();
    test_exact_fit_is_accepted();
    test_clearing_returns_to_the_library_buffer();
    test_argument_checks();
    test_the_frame_cap_still_applies();
    test_zero_size_is_rejected();
    test_dimension_bound_is_wrap_proof();
    printf("all output buffer tests passed\n");
    return 0;
}
