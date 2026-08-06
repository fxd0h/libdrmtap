/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file test_connector_names.c
 * @brief Unit test — connector type names, the prefix of drmtap_display.name
 *
 * No hardware needed: the mapping is pure, which is the point. It used to live
 * inside the enumeration loop, where only the connector types present on the
 * test machine were ever exercised, and eleven of the twenty-one types were
 * wrong (see issue #46) on hardware nobody here owns.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xf86drmMode.h>

#include "drmtap_internal.h"

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

#define EXPECT_NAME(type, expected) do { \
    const char *_got = drmtap_connector_type_name(type); \
    if (strcmp(_got, expected) != 0) { \
        fprintf(stderr, "FAIL: %s:%d: %s -> \"%s\", expected \"%s\"\n", \
                __FILE__, __LINE__, #type, _got, expected); \
        exit(1); \
    } \
} while (0)

/* The kernel's drm_connector_enum_list, spelled out again so a silent edit to
 * the library table fails here instead of shipping. These strings are what
 * /sys/class/drm/card*-<name> shows. */
static void test_every_type_matches_the_kernel_spelling(void) {
    EXPECT_NAME(DRM_MODE_CONNECTOR_Unknown,     "Unknown");
    EXPECT_NAME(DRM_MODE_CONNECTOR_VGA,         "VGA");
    EXPECT_NAME(DRM_MODE_CONNECTOR_DVII,        "DVI-I");
    EXPECT_NAME(DRM_MODE_CONNECTOR_DVID,        "DVI-D");
    EXPECT_NAME(DRM_MODE_CONNECTOR_DVIA,        "DVI-A");
    EXPECT_NAME(DRM_MODE_CONNECTOR_Composite,   "Composite");
    EXPECT_NAME(DRM_MODE_CONNECTOR_SVIDEO,      "SVIDEO");
    EXPECT_NAME(DRM_MODE_CONNECTOR_LVDS,        "LVDS");
    EXPECT_NAME(DRM_MODE_CONNECTOR_Component,   "Component");
    EXPECT_NAME(DRM_MODE_CONNECTOR_9PinDIN,     "DIN");
    EXPECT_NAME(DRM_MODE_CONNECTOR_DisplayPort, "DP");
    EXPECT_NAME(DRM_MODE_CONNECTOR_HDMIA,       "HDMI-A");
    EXPECT_NAME(DRM_MODE_CONNECTOR_HDMIB,       "HDMI-B");
    EXPECT_NAME(DRM_MODE_CONNECTOR_TV,          "TV");
    EXPECT_NAME(DRM_MODE_CONNECTOR_eDP,         "eDP");
    EXPECT_NAME(DRM_MODE_CONNECTOR_VIRTUAL,     "Virtual");
    EXPECT_NAME(DRM_MODE_CONNECTOR_DSI,         "DSI");
    EXPECT_NAME(DRM_MODE_CONNECTOR_DPI,         "DPI");
#ifdef DRM_MODE_CONNECTOR_WRITEBACK
    EXPECT_NAME(DRM_MODE_CONNECTOR_WRITEBACK,   "Writeback");
#endif
#ifdef DRM_MODE_CONNECTOR_SPI
    EXPECT_NAME(DRM_MODE_CONNECTOR_SPI,         "SPI");
#endif
#ifdef DRM_MODE_CONNECTOR_USB
    EXPECT_NAME(DRM_MODE_CONNECTOR_USB,         "USB");
#endif
}

/* The three that reach real hardware here and used to come out "Unknown":
 * LVDS is the panel of a pre-eDP laptop, DSI the panel of an ARM board, and
 * USB is both DisplayLink and the Touch Bar strip of a T2 MacBook. */
static void test_the_regressed_types_are_not_unknown(void) {
    TEST_ASSERT(strcmp(drmtap_connector_type_name(DRM_MODE_CONNECTOR_LVDS), "Unknown") != 0);
    TEST_ASSERT(strcmp(drmtap_connector_type_name(DRM_MODE_CONNECTOR_DSI), "Unknown") != 0);
#ifdef DRM_MODE_CONNECTOR_USB
    TEST_ASSERT(strcmp(drmtap_connector_type_name(DRM_MODE_CONNECTOR_USB), "Unknown") != 0);
#endif
}

/* The invariant the bug actually broke. connector_type_id counts PER TYPE, so
 * the name is unique only while distinct types keep distinct prefixes: with
 * eleven types collapsed onto "Unknown", an LVDS panel and a DSI panel both
 * enumerated as "Unknown-1" and a consumer keying on the name could not tell
 * them apart. Only the genuinely-unknown type may answer "Unknown". */
static void test_distinct_types_never_share_a_prefix(void) {
    uint32_t highest = DRM_MODE_CONNECTOR_DPI;
#ifdef DRM_MODE_CONNECTOR_WRITEBACK
    highest = DRM_MODE_CONNECTOR_WRITEBACK;
#endif
#ifdef DRM_MODE_CONNECTOR_SPI
    highest = DRM_MODE_CONNECTOR_SPI;
#endif
#ifdef DRM_MODE_CONNECTOR_USB
    highest = DRM_MODE_CONNECTOR_USB;
#endif
    for (uint32_t a = 1; a <= highest; a++) {
        const char *na = drmtap_connector_type_name(a);
        TEST_ASSERT(strcmp(na, "Unknown") != 0);
        for (uint32_t b = a + 1; b <= highest; b++) {
            if (strcmp(na, drmtap_connector_type_name(b)) == 0) {
                fprintf(stderr, "FAIL: %s:%d: types %u and %u share the name \"%s\"\n",
                        __FILE__, __LINE__, a, b, na);
                exit(1);
            }
        }
    }
}

/* A type from a kernel newer than this build must degrade, not read past the
 * table. Bounded by the array length, not by the highest macro we know. */
static void test_out_of_range_falls_back(void) {
    EXPECT_NAME(4242u, "Unknown");
    EXPECT_NAME(0xFFFFFFFFu, "Unknown");
}

int main(void) {
    test_every_type_matches_the_kernel_spelling();
    test_the_regressed_types_are_not_unknown();
    test_distinct_types_never_share_a_prefix();
    test_out_of_range_falls_back();
    printf("test_connector_names: all tests passed\n");
    return 0;
}
