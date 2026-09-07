/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file privilege_helper_stub.c
 * @brief Helper-free build of the privileged fallback (-Dhelper=disabled).
 *
 * Stand-in for privilege_helper.c in a build configured with -Dhelper=disabled.
 *
 * The point of that option is not to skip installing the helper binary — the
 * build already did that — but to leave the library with NO fork/exec path at
 * all. A consumer whose device access is arranged some other way (udev rules,
 * membership of video/render, seat management) then does not have to reason
 * about a spawn it never wanted, and `nm -D` on the .so shows no fork or exec.
 *
 * So this file deliberately contains no search paths, no fork, no exec and no
 * socketpair. It keeps the four symbols the callers reference, and each one
 * fails with the capability the caller is actually missing.
 */

#include <errno.h>
#include <string.h>

#include "drmtap_internal.h"

// One wording for every entry point: the fact the caller needs is the same.
// -EACCES and not -ENOSYS, because what it hit is that it may not read the
// scanout, which is what the rest of the library returns for that and what
// tests/test_capture.c branches on. Having no fallback is a fact about the
// build, so it goes in the message, not in the errno.
static int no_helper(drmtap_ctx *ctx) {
    drmtap_set_error(ctx,
        "this libdrmtap was built with -Dhelper=disabled, so it cannot fall "
        "back to drmtap-helper: reading the scanout needs CAP_SYS_ADMIN in the "
        "calling process (or DRM master). Grant the capability to the caller, "
        "or rebuild with the helper enabled");
    return -EACCES;
}

int drmtap_helper_spawn(drmtap_ctx *ctx) {
    return no_helper(ctx);
}

/* A no-op rather than an error: drmtap_close() calls this unconditionally, and
 * tearing down a helper that was never spawned is not a failure. */
void drmtap_helper_stop(drmtap_ctx *ctx) {
    (void)ctx;
}

int drmtap_helper_grab(drmtap_ctx *ctx, helper_grab_result_t *result,
                       void *pixel_buf, size_t buf_size) {
    (void)pixel_buf;
    (void)buf_size;
    if (result) {
        memset(result, 0, sizeof(*result));
        result->dmabuf_fd = -1;
    }
    return no_helper(ctx);
}

int drmtap_helper_get_cursor(drmtap_ctx *ctx, drmtap_cursor_info *cursor) {
    if (cursor) {
        memset(cursor, 0, sizeof(*cursor));
        cursor->pixels = NULL;
    }
    return no_helper(ctx);
}
