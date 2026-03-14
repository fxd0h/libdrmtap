/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file gpu_intel.c
 * @brief Intel GPU backend — VAAPI-accelerated deswizzle for CCS/Y-tiled FBs
 */

#include "drmtap_internal.h"

/* TODO: Phase 5 — VAAPI blit from tiled to linear surface */
