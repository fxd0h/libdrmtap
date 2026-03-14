/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file drmtap-helper.c
 * @brief Privileged helper binary — opens DRM with CAP_SYS_ADMIN and passes
 *        file descriptors to the unprivileged library via SCM_RIGHTS.
 *
 * This binary is installed to /usr/libexec/drmtap-helper and should have
 * CAP_SYS_ADMIN capability set via setcap or be launched via polkit.
 *
 * Security hardening:
 *  - Drops all capabilities except CAP_SYS_ADMIN after startup
 *  - Installs seccomp filter to allow only DRM ioctls + socket ops
 *  - Closes all file descriptors except the communication socket
 *  - Sets FD_CLOEXEC on all opened file descriptors
 */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* TODO: Phase 3 — full implementation:
     * 1. Validate we were spawned by libdrmtap (check socket fd)
     * 2. Drop capabilities except CAP_SYS_ADMIN
     * 3. Install seccomp filter
     * 4. Open DRM device
     * 5. Call drmModeGetFB2 to get handles
     * 6. Export via drmPrimeHandleToFD
     * 7. Send fd over SCM_RIGHTS
     * 8. Enter event loop: receive requests, send fds
     */

    fprintf(stderr, "drmtap-helper: stub — not yet implemented\n");
    return 1;
}
