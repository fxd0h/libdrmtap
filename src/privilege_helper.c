/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file privilege_helper.c
 * @brief Auto-spawn privileged helper and SCM_RIGHTS fd passing
 */

#include "drmtap.h"

/* TODO: Phase 3 — socketpair, fork/exec, sendmsg/recvmsg with SCM_RIGHTS */
