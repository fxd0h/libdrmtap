/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file cursor_copy.h
 * @brief Cursor readback shared by the library and privileged helper
 */

#ifndef DRMTAP_CURSOR_COPY_H
#define DRMTAP_CURSOR_COPY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <smmintrin.h>
#define DRMTAP_CURSOR_SSE41 1
#endif

#ifdef DRMTAP_CURSOR_SSE41
/* Ordinary cached loads are costly on WC cursor mappings. Keep this function
 * separate so callers retain their baseline CPU requirement. */
__attribute__((target("sse4.1")))
static void copy_cursor_streaming(uint8_t *dst, const uint8_t *src,
                                size_t row_bytes, size_t stride, uint32_t height) {
    _mm_mfence();
    for (uint32_t y = 0; y < height; y++) {
        size_t x = 0;
        for (; x + 64 <= row_bytes; x += 64) {
            __m128i a = _mm_stream_load_si128((__m128i *)(src + x));
            __m128i b = _mm_stream_load_si128((__m128i *)(src + x + 16));
            __m128i c = _mm_stream_load_si128((__m128i *)(src + x + 32));
            __m128i d = _mm_stream_load_si128((__m128i *)(src + x + 48));
            _mm_storeu_si128((__m128i *)(dst + x), a);
            _mm_storeu_si128((__m128i *)(dst + x + 16), b);
            _mm_storeu_si128((__m128i *)(dst + x + 32), c);
            _mm_storeu_si128((__m128i *)(dst + x + 48), d);
        }
        for (; x < row_bytes; x += 16) {
            __m128i pixels = _mm_stream_load_si128((__m128i *)(src + x));
            _mm_storeu_si128((__m128i *)(dst + x), pixels);
        }
        src += stride;
        dst += row_bytes;
    }
    _mm_mfence();
}
#endif

/* Repack validated cursor rows into a tightly packed destination. */
static inline void drmtap_copy_cursor_pixels(void *dst, const void *src,
                                             size_t row_bytes, size_t stride,
                                             uint32_t height) {
#ifdef DRMTAP_CURSOR_SSE41
    /* MOVNTDQA requires aligned source loads. Odd geometry and padded rows
     * remain valid through the ordinary copy path below. */
    if ((((uintptr_t)src | row_bytes | stride) & 15) == 0 &&
        __builtin_cpu_supports("sse4.1")) {
        copy_cursor_streaming(dst, src, row_bytes, stride, height);
        return;
    }
#endif
    for (uint32_t y = 0; y < height; y++) {
        memcpy((uint8_t *)dst + (size_t)y * row_bytes,
               (const uint8_t *)src + (size_t)y * stride, row_bytes);
    }
}

#undef DRMTAP_CURSOR_SSE41

#endif
