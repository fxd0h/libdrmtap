/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file stream.c
 * @brief Example: continuous screen capture at a target frame rate
 *
 * Usage:
 *   ./stream [device] [fps]
 *   ./stream /dev/dri/card0 30
 *   ./stream                    # auto-detect, 60fps
 *
 * Captures frames continuously and prints timing/format statistics.
 * This demonstrates how to use libdrmtap for real-time screen streaming
 * (e.g., for a VNC/RDP server or screen recorder).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>

#include "drmtap.h"

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

// Get monotonic time in microseconds
static uint64_t now_usec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

int main(int argc, char *argv[]) {
    const char *device = NULL;
    int target_fps = 60;

    /* Parse arguments */
    if (argc > 1) {
        device = argv[1];
    }
    if (argc > 2) {
        target_fps = atoi(argv[2]);
        if (target_fps <= 0 || target_fps > 240) {
            target_fps = 60;
        }
    }

    /* Setup signal handler for clean exit */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("libdrmtap stream example v%d.%d.%d\n",
           (drmtap_version() >> 16) & 0xFF,
           (drmtap_version() >> 8) & 0xFF,
           drmtap_version() & 0xFF);

    /* Open capture context */
    drmtap_config cfg = {0};
    cfg.device_path = device;
    cfg.debug = (getenv("DRMTAP_DEBUG") != NULL);

    drmtap_ctx *ctx = drmtap_open(&cfg);
    if (!ctx) {
        fprintf(stderr, "Error: %s\n", drmtap_error(NULL));
        return 1;
    }

    const char *driver = drmtap_gpu_driver(ctx);
    printf("GPU driver: %s\n", driver ? driver : "unknown");

    /* List displays */
    drmtap_display displays[8];
    int n_displays = drmtap_list_displays(ctx, displays, 8);
    printf("Displays found: %d\n", n_displays);
    for (int i = 0; i < n_displays && i < 8; i++) {
        printf("  [%d] %s: %ux%u@%uHz (crtc=%u, %s)\n",
               i, displays[i].name,
               displays[i].width, displays[i].height,
               displays[i].refresh_hz,
               displays[i].crtc_id,
               displays[i].active ? "active" : "inactive");
    }

    /* Capture loop */
    uint32_t frame_interval_us = 1000000u / (uint32_t)target_fps;
    uint64_t frame_count = 0;
    uint64_t total_bytes = 0;
    uint64_t start_time = now_usec();

    printf("\nCapturing at target %d fps (Ctrl+C to stop)...\n\n", target_fps);

    while (g_running) {
        uint64_t frame_start = now_usec();

        drmtap_frame_info frame = {0};
        int ret = drmtap_grab_mapped(ctx, &frame);
        if (ret < 0) {
            fprintf(stderr, "Grab failed: %s (ret=%d)\n",
                    drmtap_error(ctx), ret);
            usleep(100000);  /* Back off on error */
            continue;
        }

        frame_count++;
        total_bytes += (uint64_t)frame.stride * frame.height;

        /* Print stats every second */
        if (frame_count % (uint64_t)target_fps == 0) {
            uint64_t elapsed_us = now_usec() - start_time;
            double elapsed_s = (double)elapsed_us / 1000000.0;
            double actual_fps = (double)frame_count / elapsed_s;
            double mb_total = (double)total_bytes / (1024.0 * 1024.0);

            printf("\r  frames: %lu | fps: %.1f | %ux%u %.4s | %.0f MB captured",
                   (unsigned long)frame_count, actual_fps,
                   frame.width, frame.height,
                   (const char *)&frame.format,
                   mb_total);
            fflush(stdout);
        }

        drmtap_frame_release(ctx, &frame);

        /* Sleep to maintain target fps */
        uint64_t frame_time = now_usec() - frame_start;
        if (frame_time < frame_interval_us) {
            usleep((useconds_t)(frame_interval_us - frame_time));
        }
    }

    printf("\n\nDone. %lu frames captured.\n", (unsigned long)frame_count);

    drmtap_close(ctx);
    return 0;
}
