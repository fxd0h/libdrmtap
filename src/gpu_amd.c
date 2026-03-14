/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file gpu_amd.c
 * @brief AMD GPU backend — VAAPI + SDMA copy for DCC-compressed framebuffers
 */

#include "drmtap.h"

/* TODO: Phase 5 — VAAPI blit or SDMA copy for AMD tiled surfaces */
