/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file gpu_nvidia.c
 * @brief Nvidia GPU backend — dumb buffer export + CPU deswizzle
 */

#include "drmtap_internal.h"

/* TODO: Phase 5 — dumb buffer mmap + CPU tile decode for Nvidia block-linear */
