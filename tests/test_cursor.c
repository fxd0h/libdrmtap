/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file test_cursor.c
 * @brief Unit test — cursor helper-fallback classification (issue #58)
 *
 * No hardware needed. The direct cursor read reaches the privileged helper from
 * two places; both used to fire on any failure while claiming a privilege reason.
 * drmtap_cursor_needs_helper is the pure decision they now share: fork the helper
 * only for a genuine privilege failure, and treat a shape-change race or a
 * resource limit as a transient the next poll clears.
 *
 * Also the hotspot-provenance contract (drmtap_cursor_hotspot_valid): the case
 * that matters is a hotspot of (0, 0) which the driver really published, since
 * that is the one an "is it non-zero?" test gets wrong. No hardware can pin this
 * down either way -- a driver that exposes HOTSPOT_X/Y and parks the hotspot at
 * the image corner is not something this fleet has, and the bare-metal boxes can
 * only produce the absent case -- so the recorder the capture paths call is
 * driven directly here. What hardware DOES answer, measured separately, is that
 * the absent case reports absent (i915, no HOTSPOT_X/Y) and the present case
 * reports present (virtio-gpu).
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "drmtap.h"
#include "drmtap_internal.h"
#include "wire.h"

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

/* GETFB2 returned NULL: the errno decides. */
static void test_getfb2_null_privilege_vs_race(void) {
    /* The kernel refused the fb read for lack of privilege — the helper's job. */
    TEST_ASSERT(drmtap_cursor_needs_helper(0, EACCES, 0) == 1);
    TEST_ASSERT(drmtap_cursor_needs_helper(0, EPERM, 0) == 1);
    /* Any other errno is a retired fb_id (a cursor shape-change race) or another
     * transient: the helper cannot fix it, and the next poll clears it. */
    TEST_ASSERT(drmtap_cursor_needs_helper(0, ENOENT, 0) == 0);
    TEST_ASSERT(drmtap_cursor_needs_helper(0, EINVAL, 0) == 0);
    TEST_ASSERT(drmtap_cursor_needs_helper(0, EAGAIN, 0) == 0);
    TEST_ASSERT(drmtap_cursor_needs_helper(0, 0, 0) == 0);
}

/* GETFB2 succeeded: the presence of a GEM handle decides. */
static void test_getfb2_ok_handle_decides(void) {
    /* No handle from a successful GETFB2 is the unprivileged result — the helper
     * can read what we cannot. */
    TEST_ASSERT(drmtap_cursor_needs_helper(1, 0, 0) == 1);
    /* Had a handle but produced no pixels (prime export, mmap, the size cap, or
     * the alloc failed): transient or a resource limit, not a privilege problem. */
    TEST_ASSERT(drmtap_cursor_needs_helper(1, 0, 1) == 0);
    /* errno is meaningless once GETFB2 succeeded: a stale EACCES must not drag the
     * read that HAD a handle into a needless fork of the helper. */
    TEST_ASSERT(drmtap_cursor_needs_helper(1, EACCES, 1) == 0);
    TEST_ASSERT(drmtap_cursor_needs_helper(1, EPERM, 1) == 0);
}

/* The completeness rule, shared by the direct read and the helper read: a plane
 * that exposes one half of the pair is not a source of a hotspot. Pinned here
 * because the other reading -- either property is enough -- is the one someone
 * would reach for, and it would ship one real coordinate beside an invented
 * zero, wrong on a single axis and silent about it. */
static void test_hotspot_pair_must_be_complete(void) {
    TEST_ASSERT(wire_hot_measured(1, 1) == 1);
    TEST_ASSERT(wire_hot_measured(1, 0) == 0);
    TEST_ASSERT(wire_hot_measured(0, 1) == 0);
    TEST_ASSERT(wire_hot_measured(0, 0) == 0);
}

/* The provenance of a hotspot, which its coordinates cannot carry. */
static void test_hotspot_provenance(void) {
    drmtap_cursor_info c;
    int valid = -1;

    /* Bad arguments are told apart from every real answer. */
    memset(&c, 0, sizeof(c));
    TEST_ASSERT(drmtap_cursor_hotspot_valid(NULL, &valid) == -EINVAL);
    TEST_ASSERT(drmtap_cursor_hotspot_valid(&c, NULL) == -EINVAL);

    /* Nobody recorded an answer: a struct never filled, or filled by a build
     * that did not carry the bit (an older privileged helper). That must NOT
     * read as "not measured" -- claiming the hotspot was guessed is a claim no
     * producer made. */
    memset(&c, 0, sizeof(c));
    valid = -1;
    TEST_ASSERT(drmtap_cursor_hotspot_valid(&c, &valid) == -ENOTSUP);
    TEST_ASSERT(valid == -1);  /* untouched: -ENOTSUP is not an answer */

    /* The properties were absent, so the zeros are an absence of information. */
    memset(&c, 0, sizeof(c));
    drmtap_cursor_set_hot_provenance(&c, 0);
    valid = -1;
    TEST_ASSERT(drmtap_cursor_hotspot_valid(&c, &valid) == 0);
    TEST_ASSERT(valid == 0);

    /* THE case the whole entry point exists for: both properties were read and
     * the driver's answer is (0, 0). Identical coordinates to the line above,
     * opposite meaning, and a consumer that guesses from the bitmap here would
     * override a real measurement. */
    memset(&c, 0, sizeof(c));
    c.hot_x = 0;
    c.hot_y = 0;
    drmtap_cursor_set_hot_provenance(&c, 1);
    valid = -1;
    TEST_ASSERT(drmtap_cursor_hotspot_valid(&c, &valid) == 0);
    TEST_ASSERT(valid == 1);
    TEST_ASSERT(c.hot_x == 0 && c.hot_y == 0);  /* the answer moves nothing */

    /* A measured non-zero hotspot, the ordinary para-virtualized case. */
    memset(&c, 0, sizeof(c));
    c.hot_x = 4;
    c.hot_y = 7;
    drmtap_cursor_set_hot_provenance(&c, 1);
    valid = -1;
    TEST_ASSERT(drmtap_cursor_hotspot_valid(&c, &valid) == 0);
    TEST_ASSERT(valid == 1);

    /* Releasing the sample takes the answer with it: a struct reused for a
     * capture that fails before recording must not answer for the last one. */
    drmtap_cursor_release(NULL, &c);
    valid = -1;
    TEST_ASSERT(drmtap_cursor_hotspot_valid(&c, &valid) == -ENOTSUP);
}

int main(void) {
    test_getfb2_null_privilege_vs_race();
    test_getfb2_ok_handle_decides();
    test_hotspot_pair_must_be_complete();
    test_hotspot_provenance();
    printf("test_cursor: all passed\n");
    return 0;
}
