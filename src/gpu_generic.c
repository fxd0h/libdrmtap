/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file gpu_generic.c
 * @brief Generic/VM backend for linear framebuffers (virtio, vmwgfx, vbox, vkms)
 */

#include "drmtap_internal.h"

/* TODO: Phase 2 — linear mmap path (simplest backend, no deswizzle) */
