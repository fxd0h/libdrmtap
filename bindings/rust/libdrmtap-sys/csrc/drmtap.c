/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file drmtap.c
 * @brief Context management, DRM device open, error handling, and debug logging
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <linux/capability.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drmtap_internal.h"

/* True if this thread holds CAP_SYS_ADMIN in its effective set. drmModeGetFB2
 * returns framebuffer handles without DRM master only for a caller that holds
 * CAP_SYS_ADMIN; a process running as uid 0 that has dropped CAP_SYS_ADMIN does
 * NOT qualify and relies on implicit master exactly like an unprivileged caller.
 * The raw capget syscall is used so the library links no libcap and adds nothing
 * to the DT_NEEDED of the .so the privileged service dlopens. On any error the
 * caller is treated as unprivileged (keep master), which is the safe default. */
static int drmtap_have_cap_sys_admin(void) {
    struct __user_cap_header_struct hdr = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid = 0, /* 0 == self */
    };
    struct __user_cap_data_struct data[2] = {{0, 0, 0}, {0, 0, 0}};
    if (syscall(SYS_capget, &hdr, data) != 0) {
        /* capget is unavailable (e.g. a seccomp filter denies the syscall while
         * still permitting getuid). Fall back to the uid check so a genuine root
         * capture service still drops master -- this matches the 0.4.13 gate and
         * avoids regressing the VT-switch blackout fix when capget is filtered. */
        return (geteuid() == 0 || getuid() == 0);
    }
    /* CAP_SYS_ADMIN (21) lives in the first 32-bit word. */
    return (data[0].effective & (1u << CAP_SYS_ADMIN)) != 0;
}

/* Error string for when ctx is NULL (from failed drmtap_open). Thread-local so
 * concurrent NULL-ctx failures on different threads don't race on one buffer. */
static _Thread_local char g_static_error[512] = "";

/* ========================================================================= */
/* Internal helpers (exported via drmtap_internal.h)                         */
/* ========================================================================= */

void drmtap_set_error(drmtap_ctx *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (ctx) {
        vsnprintf(ctx->error_msg, sizeof(ctx->error_msg), fmt, ap);
    } else {
        vsnprintf(g_static_error, sizeof(g_static_error), fmt, ap);
    }
    va_end(ap);
}

void drmtap_debug_log(drmtap_ctx *ctx, const char *fmt, ...) {
    if (!ctx || !ctx->debug) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[drmtap] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ========================================================================= */
/* DRM device discovery                                                      */
/* ========================================================================= */

// Auto-detect DRM device: scan /dev/dri/card* for one with KMS resources
static int open_drm_auto(drmtap_ctx *ctx) {
    char path[64];

    /* Check DRM_DEVICE env var first, but ONLY when unprivileged. A privileged
     * capture service (root / effective-root, e.g. the unattended --service)
     * must not let an attacker-influenceable environment variable redirect which
     * device it opens; it relies on the explicit config device_path or the KMS
     * auto-scan below instead. DRM_DEVICE stays honored for unprivileged test /
     * dev runs. */
    const char *env_dev = NULL;
    if (getuid() != 0 && geteuid() != 0) {
        env_dev = getenv("DRM_DEVICE");
    }
    if (env_dev) {
        drmtap_debug_log(ctx, "trying DRM_DEVICE=%s", env_dev);
        int fd = open(env_dev, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            snprintf(ctx->device_path, sizeof(ctx->device_path), "%s", env_dev);
            return fd;
        }
        drmtap_debug_log(ctx, "DRM_DEVICE open failed: %s", strerror(errno));
    }

    /* Two-pass scan of /dev/dri/card0..card15:
     *   Pass 1: find the device driving the MOST displays (active CRTCs)
     *   Pass 2: fallback to any device with KMS resources
     *
     * A context covers ONE device, so on a multi-GPU host this pick decides
     * which displays are capturable at all; drmtap_list_devices() is the way to
     * reach the others. Until 0.4.15 this took the FIRST card with any active
     * CRTC, which is just "lowest minor wins" and can be badly wrong: load vkms
     * next to a real GPU and it registers as card0 with one 1024x768 virtual
     * output, so auto-detect captured that instead of the three real monitors on
     * card1. Ranking by active-CRTC count makes the single-card choice land on
     * the GPU actually driving the desktop. It remains a heuristic — a caller
     * that wants every display must enumerate devices.
     */
    int fallback_fd = -1;
    char fallback_path[64] = {0};
    int best_fd = -1;
    int best_active = 0;
    char best_path[64] = {0};

    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        drmtap_debug_log(ctx, "probing %s", path);

        drmModeRes *res = drmModeGetResources(fd);
        if (!res) {
            drmtap_debug_log(ctx, "  no KMS resources, skipping");
            close(fd);
            continue;
        }

        /* Count active CRTCs (monitors this device is driving) */
        int active = 0;
        for (int j = 0; j < res->count_crtcs; j++) {
            drmModeCrtc *crtc = drmModeGetCrtc(fd, res->crtcs[j]);
            if (crtc) {
                if (crtc->mode_valid && crtc->buffer_id > 0) {
                    drmtap_debug_log(ctx, "  CRTC %u active (%dx%d)",
                                     crtc->crtc_id, crtc->width, crtc->height);
                    active++;
                }
                drmModeFreeCrtc(crtc);
            }
        }
        drmModeFreeResources(res);

        if (active > 0) {
            /* Keep the best candidate so far; ties keep the lower minor. */
            if (active > best_active) {
                if (best_fd >= 0) {
                    close(best_fd);
                }
                best_fd = fd;
                best_active = active;
                snprintf(best_path, sizeof(best_path), "%s", path);
            } else {
                close(fd);
            }
            continue;
        }

        /* Keep as fallback (first device with KMS but no active CRTC) */
        if (fallback_fd < 0) {
            fallback_fd = fd;
            snprintf(fallback_path, sizeof(fallback_path), "%s", path);
            drmtap_debug_log(ctx, "  no active CRTC, saved as fallback");
        } else {
            close(fd);
        }
    }

    /* The GPU driving the most displays wins. */
    if (best_fd >= 0) {
        if (fallback_fd >= 0) {
            close(fallback_fd);
        }
        snprintf(ctx->device_path, sizeof(ctx->device_path), "%s", best_path);
        drmtap_debug_log(ctx, "using %s (%d active display(s))", best_path,
                         best_active);
        return best_fd;
    }

    /* No device with active CRTC found — use fallback if available */
    if (fallback_fd >= 0) {
        snprintf(ctx->device_path, sizeof(ctx->device_path), "%s", fallback_path);
        drmtap_debug_log(ctx, "using fallback device %s (no active CRTC found)", fallback_path);
        return fallback_fd;
    }

    return -1;
}

// Detect GPU driver name from the DRM fd
static void detect_driver(drmtap_ctx *ctx) {
    drmVersion *ver = drmGetVersion(ctx->drm_fd);
    if (ver) {
        snprintf(ctx->driver_name, sizeof(ctx->driver_name), "%.*s",
                 ver->name_len, ver->name);
        drmtap_debug_log(ctx, "GPU driver: %s (v%d.%d.%d)",
                         ctx->driver_name, ver->version_major,
                         ver->version_minor, ver->version_patchlevel);
        drmFreeVersion(ver);
    } else {
        drmtap_debug_log(ctx, "warning: could not get DRM version");
    }
}

/* ========================================================================= */
/* Public API                                                                */
/* ========================================================================= */

int drmtap_version(void) {
    return (DRMTAP_VERSION_MAJOR << 16) |
           (DRMTAP_VERSION_MINOR << 8) |
           DRMTAP_VERSION_PATCH;
}

drmtap_ctx *drmtap_open(const drmtap_config *config) {
    drmtap_ctx *ctx = calloc(1, sizeof(drmtap_ctx));
    if (!ctx) {
        drmtap_set_error(NULL, "Failed to allocate context: %s",
                         strerror(errno));
        return NULL;
    }

    ctx->drm_fd = -1;
    ctx->helper_pid = -1;
    ctx->helper_fd = -1;
    
    for (int i = 0; i < DRMTAP_FAST_SLOTS; i++) {
        ctx->fast_slots[i].prime_fd = -1;
    }

    /* Parse config */
    if (config) {
        ctx->debug = config->debug;
        ctx->crtc_id = config->crtc_id;

        if (config->device_path) {
            snprintf(ctx->device_path, sizeof(ctx->device_path),
                     "%s", config->device_path);
        }
        if (config->helper_path) {
            snprintf(ctx->helper_path, sizeof(ctx->helper_path),
                     "%s", config->helper_path);
        }
    }

    /* Check DRMTAP_DEBUG env var */
    const char *dbg_env = getenv("DRMTAP_DEBUG");
    if (dbg_env && dbg_env[0] == '1') {
        ctx->debug = 1;
    }

    drmtap_debug_log(ctx, "drmtap v%d.%d.%d opening (pid=%d uid=%d)",
                     DRMTAP_VERSION_MAJOR, DRMTAP_VERSION_MINOR,
                     DRMTAP_VERSION_PATCH, getpid(), getuid());

    /* Open DRM device */
    drmtap_debug_log(ctx, "device_path=[%s] DRM_DEVICE=[%s]", ctx->device_path, getenv("DRM_DEVICE") ? getenv("DRM_DEVICE") : "(null)");
    if (ctx->device_path[0]) {
        /* Explicit device path */
        ctx->drm_fd = open(ctx->device_path, O_RDWR | O_CLOEXEC);
        if (ctx->drm_fd < 0) {
            drmtap_set_error(NULL, "Failed to open %s: %s",
                             ctx->device_path, strerror(errno));
            drmtap_debug_log(ctx, "open(%s) FAILED: %s", ctx->device_path, strerror(errno));
        goto fail;
        }
    } else {
        /* Auto-detect */
        ctx->drm_fd = open_drm_auto(ctx);
        if (ctx->drm_fd < 0) {
            drmtap_set_error(NULL, "No DRM device found — is a GPU attached?");
            goto fail;
        }
    }

    /* ── Protect fd from async runtime hijacking ──
     * Async runtimes (tokio, etc.) can close/reuse low-numbered fds.
     * Duplicate our DRM fd to a number >= 100 so it's safe. */
    {
        int high_fd = fcntl(ctx->drm_fd, F_DUPFD_CLOEXEC, 100);
        if (high_fd >= 0) {
            close(ctx->drm_fd);
            ctx->drm_fd = high_fd;
        }
        /* If fcntl fails, keep the original fd (best effort) */
    }

    /* Defensively drop DRM master -- but ONLY when we hold CAP_SYS_ADMIN. We only
     * READ scanout and never modeset, so if a process holding CAP_SYS_ADMIN opened
     * the node while no client held master (e.g. an unattended capture service that
     * started at boot before the compositor), the kernel granted it implicit master,
     * which would then block the compositor from acquiring master on a VT switch ->
     * a black/frozen display; dropping it is safe there because drmModeGetFB2 still
     * returns handles via CAP_SYS_ADMIN. A caller WITHOUT CAP_SYS_ADMIN -- including a
     * uid-0 process that dropped it -- RELIES on that implicit master for drmModeGetFB2
     * to return framebuffer handles at all, so it must keep it. The gate is the
     * capability, not the uid: uid 0 without CAP_SYS_ADMIN cannot use GetFB2 after
     * losing master. drmDropMaster returns 0 only when we actually held master; when a
     * compositor already holds it (the normal desktop case) it is a harmless no-op. */
    if (drmtap_have_cap_sys_admin() &&
        drmDropMaster(ctx->drm_fd) == 0) {
        drmtap_debug_log(ctx, "dropped implicit DRM master on %s", ctx->device_path);
    }

    /* Detect GPU driver */
    detect_driver(ctx);

    /* Set universal planes — needed for cursor plane detection */
    if (drmSetClientCap(ctx->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0) {
        drmtap_debug_log(ctx,
                         "warning: DRM_CLIENT_CAP_UNIVERSAL_PLANES not supported");
    }

    /* Enable atomic modesetting uAPI — best-effort, read-only. This exposes the
     * connector CRTC_ID property (atomic-flagged, hidden from non-atomic
     * clients) so enumeration can resolve the real scanout CRTC of a
     * compositor-managed connector whose legacy encoder link reports 0. We never
     * commit; the cap only widens property visibility. Harmless if unsupported. */
    if (drmSetClientCap(ctx->drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0) {
        drmtap_debug_log(ctx,
                         "warning: DRM_CLIENT_CAP_ATOMIC not supported "
                         "(connector CRTC_ID fallback unavailable)");
    }

    drmtap_debug_log(ctx, "context opened: %s (%s)",
                     ctx->device_path, ctx->driver_name);

    /* TODO: Phase 3 — locate and spawn helper if needed */

    return ctx;

fail:
    if (ctx->drm_fd >= 0) {
        close(ctx->drm_fd);
    }
    free(ctx);
    return NULL;
}

int drmtap_list_devices(drmtap_device *out, int max_count) {
    if (!out || max_count <= 0) {
        return -EINVAL;
    }

    int found = 0;
    for (int i = 0; i < 16 && found < max_count; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        /* Opening a card node while nobody holds master grants US implicit
         * master, which would block a compositor from acquiring it on a VT
         * switch. Enumeration never modesets, so give it straight back — same
         * reasoning as drmtap_open, and the gate is the capability, not the uid
         * (drmModeGetResources works without master either way). */
        if (drmtap_have_cap_sys_admin()) {
            drmDropMaster(fd);
        }

        drmModeRes *res = drmModeGetResources(fd);
        if (!res) {
            close(fd);   /* render-only or no KMS: not a capturable device */
            continue;
        }

        drmtap_device *dev = &out[found];
        memset(dev, 0, sizeof(*dev));
        snprintf(dev->path, sizeof(dev->path), "%s", path);

        /* Count CRTCs actually scanning out. Deliberately NOT walking the
         * connectors: drmModeGetConnector re-probes the link (DDC), and doing
         * that across every card of every GPU just to enumerate can disturb a
         * live display. An active CRTC is also the exact thing a capture needs. */
        for (int j = 0; j < res->count_crtcs; j++) {
            drmModeCrtc *crtc = drmModeGetCrtc(fd, res->crtcs[j]);
            if (!crtc) {
                continue;
            }
            if (crtc->mode_valid && crtc->buffer_id > 0) {
                dev->display_count++;
            }
            drmModeFreeCrtc(crtc);
        }
        drmModeFreeResources(res);

        drmVersionPtr ver = drmGetVersion(fd);
        if (ver) {
            if (ver->name) {
                snprintf(dev->driver, sizeof(dev->driver), "%s", ver->name);
            }
            drmFreeVersion(ver);
        }

        drmDevicePtr dd = NULL;
        if (drmGetDevice2(fd, 0, &dd) == 0 && dd) {
            if ((dd->available_nodes & (1 << DRM_NODE_RENDER)) &&
                dd->nodes[DRM_NODE_RENDER]) {
                snprintf(dev->render_node, sizeof(dev->render_node), "%s",
                         dd->nodes[DRM_NODE_RENDER]);
            }
            drmFreeDevice(&dd);
        }

        close(fd);
        found++;
    }

    return found;
}

/* ── Render-node selection on a multi-GPU box ─────────────────────────────
 *
 * "The first openable /dev/dri/renderD*" is the wrong default when more than
 * one GPU is present: the scanout DMA-BUF is exported by the card that drives
 * the display, and importing it into a DIFFERENT vendor's render node can fail
 * outright (incompatible tiling modifiers) — permanently, since the choice is
 * made once at open. A Jetson Orin shows this concretely: card1 is `tegra`
 * (renderD128, NO connectors) while card2 is `nvidia-drm` (renderD129) and owns
 * the connected DP-1, so the old scan picked exactly the node that does not own
 * the scanout.
 *
 * The unprivileged converter cannot simply open the KMS cards to find out which
 * one drives a display — it may hold no rights on them — so the ranking is read
 * from sysfs, which needs no privilege: a card whose connector is `connected`
 * AND `enabled` is actively scanning out and wins; merely `connected` is the
 * runner-up; a card with no outputs (a compute/offload GPU) is never preferred.
 * When sysfs is unavailable (a container without /sys) this yields nothing and
 * the caller falls back to the historical first-openable scan.
 *
 * A caller that KNOWS the exporting device should not rely on this heuristic at
 * all — drmtap_render_node() on the capture context names the exact node.
 */

/* Read the first line of a small sysfs file into `buf` (NUL-terminated).
 * Returns 0 on success. */
static int read_sysfs_line(const char *path, char *buf, size_t len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) {
        *nl = '\0';
    }
    return 0;
}

/* Name of the KMS card ("card1") backing render node `render_name`
 * ("renderD128"), via /sys/class/drm/<render>/device/drm/. Returns 0 on
 * success. */
static int card_for_render_node(const char *render_name, char *out,
                                size_t out_len) {
    char dir_path[320];
    snprintf(dir_path, sizeof(dir_path), "/sys/class/drm/%s/device/drm",
             render_name);
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return -1;
    }
    int found = -1;
    const struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        /* The directory holds the sibling nodes of the same device: cardN,
         * renderDM and (on older kernels) controlDK. Take the primary node —
         * "card" with no "-CONNECTOR" suffix. */
        if (strncmp(ent->d_name, "card", 4) != 0 || !ent->d_name[4] ||
            strchr(ent->d_name, '-') != NULL) {
            continue;
        }
        size_t len = strlen(ent->d_name);
        if (len >= out_len) {
            continue;  /* not a name we could have produced; ignore it */
        }
        memcpy(out, ent->d_name, len + 1);
        found = 0;
        break;
    }
    closedir(dir);
    return found;
}

/* How strongly card `card_name` looks like the device driving a display:
 * 2 = has a connected AND enabled connector (actively scanning out),
 * 1 = has a connected connector, 0 = none (or sysfs unreadable). */
static int card_output_rank(const char *card_name) {
    DIR *dir = opendir("/sys/class/drm");
    if (!dir) {
        return 0;
    }
    size_t card_len = strlen(card_name);
    int rank = 0;
    const struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        /* Connectors are exposed as "<card>-<CONNECTOR>", e.g. card1-DP-1. */
        if (strncmp(ent->d_name, card_name, card_len) != 0 ||
            ent->d_name[card_len] != '-') {
            continue;
        }
        char path[320], val[32];
        snprintf(path, sizeof(path), "/sys/class/drm/%s/status", ent->d_name);
        /* "disconnected" also ends in "connected" — compare from the start. */
        if (read_sysfs_line(path, val, sizeof(val)) != 0 ||
            strcmp(val, "connected") != 0) {
            continue;
        }
        if (rank < 1) {
            rank = 1;
        }
        snprintf(path, sizeof(path), "/sys/class/drm/%s/enabled", ent->d_name);
        if (read_sysfs_line(path, val, sizeof(val)) == 0 &&
            strcmp(val, "enabled") == 0) {
            rank = 2;
            break;
        }
    }
    closedir(dir);
    return rank;
}

/* Best-ranked render node path into `out`. Returns 0 when one was picked. */
static int pick_scanout_render_node(char *out, size_t out_len) {
    DIR *dir = opendir("/sys/class/drm");
    if (!dir) {
        return -1;
    }
    int best_rank = 0;
    char best[64] = {0};
    const struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "renderD", 7) != 0) {
            continue;
        }
        char card[64];
        if (card_for_render_node(ent->d_name, card, sizeof(card)) != 0) {
            continue;
        }
        size_t len = strlen(ent->d_name);
        if (len >= sizeof(best)) {
            continue;
        }
        int rank = card_output_rank(card);
        if (rank > best_rank) {
            best_rank = rank;
            memcpy(best, ent->d_name, len + 1);
            if (rank >= 2) {
                break;  /* actively scanning out — nothing can outrank it */
            }
        }
    }
    closedir(dir);
    if (best_rank == 0) {
        return -1;
    }
    snprintf(out, out_len, "/dev/dri/%s", best);
    return 0;
}

const char *drmtap_render_node(drmtap_ctx *ctx) {
    if (!ctx || ctx->drm_fd < 0) {
        return NULL;
    }
    if (ctx->render_node[0]) {
        return ctx->render_node;
    }
    drmDevicePtr dev = NULL;
    if (drmGetDevice2(ctx->drm_fd, 0, &dev) != 0 || !dev) {
        return NULL;
    }
    if ((dev->available_nodes & (1 << DRM_NODE_RENDER)) &&
        dev->nodes[DRM_NODE_RENDER]) {
        snprintf(ctx->render_node, sizeof(ctx->render_node), "%s",
                 dev->nodes[DRM_NODE_RENDER]);
    }
    drmFreeDevice(&dev);
    return ctx->render_node[0] ? ctx->render_node : NULL;
}

drmtap_ctx *drmtap_open_render(const char *render_node) {
    drmtap_ctx *ctx = calloc(1, sizeof(drmtap_ctx));
    if (!ctx) {
        drmtap_set_error(NULL, "Failed to allocate context: %s",
                         strerror(errno));
        return NULL;
    }

    ctx->drm_fd = -1;
    ctx->helper_pid = -1;
    ctx->helper_fd = -1;
    ctx->is_render_only = 1;
    for (int i = 0; i < DRMTAP_FAST_SLOTS; i++) {
        ctx->fast_slots[i].prime_fd = -1;
    }

    const char *dbg_env = getenv("DRMTAP_DEBUG");
    if (dbg_env && dbg_env[0] == '1') {
        ctx->debug = 1;
    }

    drmtap_debug_log(ctx, "drmtap v%d.%d.%d opening render-only (pid=%d uid=%d)",
                     DRMTAP_VERSION_MAJOR, DRMTAP_VERSION_MINOR,
                     DRMTAP_VERSION_PATCH, getpid(), getuid());

    if (render_node && render_node[0]) {
        snprintf(ctx->device_path, sizeof(ctx->device_path), "%s", render_node);
        ctx->drm_fd = open(render_node, O_RDWR | O_CLOEXEC);
        if (ctx->drm_fd < 0) {
            drmtap_set_error(NULL, "Failed to open %s: %s",
                             render_node, strerror(errno));
            goto fail;
        }
    } else {
        /* Auto-detect. Prefer the render node of the card that actually drives
         * a display (see pick_scanout_render_node): on a multi-GPU box that is
         * the device exporting the scanout we will be asked to import, and the
         * only one guaranteed to understand its tiling modifier. */
        char preferred[96];
        if (pick_scanout_render_node(preferred, sizeof(preferred)) == 0) {
            int fd = open(preferred, O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                snprintf(ctx->device_path, sizeof(ctx->device_path), "%s",
                         preferred);
                ctx->drm_fd = fd;
                drmtap_debug_log(ctx, "render node %s selected: its card "
                                 "drives a display", preferred);
            } else {
                drmtap_debug_log(ctx, "render node %s drives a display but "
                                 "could not be opened (%s); scanning",
                                 preferred, strerror(errno));
            }
        }
    }

    if (ctx->drm_fd < 0 && !(render_node && render_node[0])) {
        /* Fallback: first openable render node. DRM render minors span the
         * whole 128..191 range (up to 64 nodes on a multi-GPU box), so scan all
         * of it — a valid device can sit above renderD143. No KMS probing here
         * on purpose: a render node has no resources. */
        for (int i = 128; i < 128 + 64; i++) {
            char path[64];
            snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                snprintf(ctx->device_path, sizeof(ctx->device_path),
                         "%s", path);
                ctx->drm_fd = fd;
                break;
            }
        }
        if (ctx->drm_fd < 0) {
            drmtap_set_error(NULL,
                "No DRM render node found (/dev/dri/renderD*)");
            goto fail;
        }
    }

    /* Same low-fd protection as drmtap_open: async runtimes can close/reuse
     * low-numbered fds. */
    {
        int high_fd = fcntl(ctx->drm_fd, F_DUPFD_CLOEXEC, 100);
        if (high_fd >= 0) {
            close(ctx->drm_fd);
            ctx->drm_fd = high_fd;
        }
    }

    /* The driver name selects the CPU deswizzle fallback when EGL is out. */
    detect_driver(ctx);

    drmtap_debug_log(ctx, "render context opened: %s (%s)",
                     ctx->device_path, ctx->driver_name);
    return ctx;

fail:
    if (ctx->drm_fd >= 0) {
        close(ctx->drm_fd);
    }
    free(ctx);
    return NULL;
}

void drmtap_close(drmtap_ctx *ctx) {
    if (!ctx) {
        return;
    }

    drmtap_debug_log(ctx, "closing context");

    /* Clean up persistent fast-grab state */
    drmtap_fast_cleanup(ctx);

    /* Release this thread's EGL detile context (context + shader + FBO + linear
     * texture). drmtap_close runs on the capture thread that built it, and C
     * thread-local storage has no destructor, so without this every open/close on
     * a fresh capture thread leaks a full EGL context (~tens of MB). No-op if this
     * thread never used the EGL path. */
    drmtap_gpu_egl_thread_cleanup();

    /* Stop helper if running */
    drmtap_helper_stop(ctx);

    if (ctx->drm_fd >= 0) {
        close(ctx->drm_fd);
        ctx->drm_fd = -1;
    }

    /* Context-owned, reused-across-frames buffers (the deswizzle/EGL shadow and
     * the helper-mode pixel receive buffer) must be released here —
     * frame_release only drops per-frame state. */
    free(ctx->deswizzle_buf);
    ctx->deswizzle_buf = NULL;
    ctx->deswizzle_buf_size = 0;
    free(ctx->pixel_buf);
    ctx->pixel_buf = NULL;
    ctx->pixel_buf_size = 0;

    free(ctx);
}

const char *drmtap_error(drmtap_ctx *ctx) {
    if (ctx) {
        return ctx->error_msg[0] ? ctx->error_msg : NULL;
    }
    return g_static_error[0] ? g_static_error : NULL;
}

const char *drmtap_gpu_driver(drmtap_ctx *ctx) {
    if (!ctx || !ctx->driver_name[0]) {
        return NULL;
    }
    return ctx->driver_name;
}

int drmtap_drm_fd(drmtap_ctx *ctx) {
    if (!ctx) {
        return -1;
    }
    return ctx->drm_fd;
}
