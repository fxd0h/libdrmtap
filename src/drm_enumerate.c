/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file drm_enumerate.c
 * @brief Plane, CRTC, and connector enumeration via DRM/KMS
 *
 * Enumerates connected displays and their properties using the DRM/KMS
 * modesetting API. Each display is a connector-CRTC pair with resolution,
 * refresh rate, and active status.
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drmtap_internal.h"

/* ========================================================================= */
/* Public API                                                                */
/* ========================================================================= */

int drmtap_list_displays(drmtap_ctx *ctx, drmtap_display *out, int max_count) {
    if (!ctx || !out || max_count <= 0) {
        return -EINVAL;
    }

    if (ctx->drm_fd < 0) {
        return -ENODEV;
    }

    drmModeRes *res = drmModeGetResources(ctx->drm_fd);
    if (!res) {
        return -ENODEV;
    }

    int found = 0;

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(ctx->drm_fd,
                                                     res->connectors[i]);
        if (!conn) {
            continue;
        }

        /* Skip disconnected connectors */
        if (conn->connection != DRM_MODE_CONNECTED) {
            drmModeFreeConnector(conn);
            continue;
        }

        /* Find the CRTC attached to this connector */
        uint32_t crtc_id = 0;
        if (conn->encoder_id) {
            drmModeEncoder *enc = drmModeGetEncoder(ctx->drm_fd,
                                                    conn->encoder_id);
            if (enc) {
                crtc_id = enc->crtc_id;
                drmModeFreeEncoder(enc);
            }
        }

        /* Fill output entry */
        if (found < max_count) {
            drmtap_display *d = &out[found];
            memset(d, 0, sizeof(*d));

            d->connector_id = conn->connector_id;
            d->crtc_id = crtc_id;
            d->active = (crtc_id != 0) ? 1 : 0;

            /* Build connector name: type + type_id (e.g., "HDMI-A-1") */
            const char *type_name;
            switch (conn->connector_type) {
                case DRM_MODE_CONNECTOR_VGA:
                    type_name = "VGA"; break;
                case DRM_MODE_CONNECTOR_DVII:
                    type_name = "DVI-I"; break;
                case DRM_MODE_CONNECTOR_DVID:
                    type_name = "DVI-D"; break;
                case DRM_MODE_CONNECTOR_DVIA:
                    type_name = "DVI-A"; break;
                case DRM_MODE_CONNECTOR_HDMIA:
                    type_name = "HDMI-A"; break;
                case DRM_MODE_CONNECTOR_HDMIB:
                    type_name = "HDMI-B"; break;
                case DRM_MODE_CONNECTOR_DisplayPort:
                    type_name = "DP"; break;
                case DRM_MODE_CONNECTOR_eDP:
                    type_name = "eDP"; break;
                case DRM_MODE_CONNECTOR_VIRTUAL:
                    type_name = "Virtual"; break;
                default:
                    type_name = "Unknown"; break;
            }
            snprintf(d->name, sizeof(d->name), "%s-%u",
                     type_name, conn->connector_type_id);

            /* Get current mode from the CRTC */
            if (crtc_id) {
                drmModeCrtc *crtc = drmModeGetCrtc(ctx->drm_fd, crtc_id);
                if (crtc) {
                    if (crtc->mode_valid) {
                        d->width = crtc->mode.hdisplay;
                        d->height = crtc->mode.vdisplay;
                        d->refresh_hz = crtc->mode.vrefresh;
                    }
                    drmModeFreeCrtc(crtc);
                }
            }

            /* If no active mode, use preferred mode from connector */
            if (d->width == 0 && conn->count_modes > 0) {
                for (int m = 0; m < conn->count_modes; m++) {
                    if (conn->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
                        d->width = conn->modes[m].hdisplay;
                        d->height = conn->modes[m].vdisplay;
                        d->refresh_hz = conn->modes[m].vrefresh;
                        break;
                    }
                }
                /* Fallback to first mode */
                if (d->width == 0) {
                    d->width = conn->modes[0].hdisplay;
                    d->height = conn->modes[0].vdisplay;
                    d->refresh_hz = conn->modes[0].vrefresh;
                }
            }

            drmtap_debug_log(ctx, "display [%d] %s: %ux%u@%uHz crtc=%u %s",
                             found, d->name, d->width, d->height,
                             d->refresh_hz, d->crtc_id,
                             d->active ? "(active)" : "(inactive)");
        }

        found++;
        drmModeFreeConnector(conn);
    }

    /* Cache counts for hotplug detection */
    ctx->cached_connector_count = (uint32_t)res->count_connectors;
    ctx->cached_crtc_count = (uint32_t)res->count_crtcs;

    drmModeFreeResources(res);
    return found;
}

int drmtap_displays_changed(drmtap_ctx *ctx) {
    if (!ctx) {
        return -EINVAL;
    }

    if (ctx->drm_fd < 0) {
        return -ENODEV;
    }

    drmModeRes *res = drmModeGetResources(ctx->drm_fd);
    if (!res) {
        return -ENODEV;
    }

    int changed = 0;
    if ((uint32_t)res->count_connectors != ctx->cached_connector_count ||
        (uint32_t)res->count_crtcs != ctx->cached_crtc_count) {
        changed = 1;
    }

    /* Update cache */
    ctx->cached_connector_count = (uint32_t)res->count_connectors;
    ctx->cached_crtc_count = (uint32_t)res->count_crtcs;

    drmModeFreeResources(res);
    return changed;
}
