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

/* The kernel's own connector-type strings, from drm_connector_enum_list in
 * drivers/gpu/drm/drm_connector.c. They are what /sys/class/drm/card*-<name>
 * is built from, so a caller can correlate a display with sysfs, and they are
 * the prefix that makes the name unique: connector_type_id counts PER TYPE, so
 * two types sharing one prefix would also share their id space and produce two
 * displays with the same name (an LVDS panel and a DSI panel both "Unknown-1").
 *
 * libdrm exposes the same table as drmModeGetConnectorTypeName(), but only
 * since 2.4.112 and our floor is 2.4.95, hence the local copy. The last three
 * macros arrived after that floor, so they are guarded: at 2.4.95 the array
 * simply ends earlier and those types take the "Unknown" fallback. */
static const char *const connector_type_names[] = {
    [DRM_MODE_CONNECTOR_Unknown]     = "Unknown",
    [DRM_MODE_CONNECTOR_VGA]         = "VGA",
    [DRM_MODE_CONNECTOR_DVII]        = "DVI-I",
    [DRM_MODE_CONNECTOR_DVID]        = "DVI-D",
    [DRM_MODE_CONNECTOR_DVIA]        = "DVI-A",
    [DRM_MODE_CONNECTOR_Composite]   = "Composite",
    [DRM_MODE_CONNECTOR_SVIDEO]      = "SVIDEO",
    [DRM_MODE_CONNECTOR_LVDS]        = "LVDS",
    [DRM_MODE_CONNECTOR_Component]   = "Component",
    [DRM_MODE_CONNECTOR_9PinDIN]     = "DIN",
    [DRM_MODE_CONNECTOR_DisplayPort] = "DP",
    [DRM_MODE_CONNECTOR_HDMIA]       = "HDMI-A",
    [DRM_MODE_CONNECTOR_HDMIB]       = "HDMI-B",
    [DRM_MODE_CONNECTOR_TV]          = "TV",
    [DRM_MODE_CONNECTOR_eDP]         = "eDP",
    [DRM_MODE_CONNECTOR_VIRTUAL]     = "Virtual",
    [DRM_MODE_CONNECTOR_DSI]         = "DSI",
    [DRM_MODE_CONNECTOR_DPI]         = "DPI",
#ifdef DRM_MODE_CONNECTOR_WRITEBACK
    [DRM_MODE_CONNECTOR_WRITEBACK]   = "Writeback",
#endif
#ifdef DRM_MODE_CONNECTOR_SPI
    [DRM_MODE_CONNECTOR_SPI]         = "SPI",
#endif
#ifdef DRM_MODE_CONNECTOR_USB
    [DRM_MODE_CONNECTOR_USB]         = "USB",
#endif
};

const char *drmtap_connector_type_name(uint32_t connector_type) {
    if (connector_type < sizeof(connector_type_names) / sizeof(*connector_type_names)
        && connector_type_names[connector_type]) {
        return connector_type_names[connector_type];
    }
    /* A type a newer kernel added, or one this build's headers predate. */
    return "Unknown";
}

/* Resolve the CRTC bound to a connector via the atomic CRTC_ID property.
 *
 * The legacy path (drmModeGetEncoder(conn->encoder_id)->crtc_id) reports 0 for a
 * compositor-managed / atomically-bound connector — a documented weakness of the
 * legacy encoder API under atomic KMS. A connector that enumerates crtc_id==0 is
 * then mishandled downstream (open() treats 0 as "auto-select the first CRTC",
 * silently capturing the wrong monitor). Fall back to the connector's atomic
 * CRTC_ID property to get the real scanout CRTC. Returns 0 if the connector is
 * genuinely unbound or the property is unavailable (e.g. no atomic cap). */
static uint32_t connector_crtc_from_atomic(int fd, uint32_t connector_id) {
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (!props) {
        return 0;
    }
    uint32_t crtc_id = 0;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (!p) {
            continue;
        }
        if (strcmp(p->name, "CRTC_ID") == 0) {
            crtc_id = (uint32_t)props->prop_values[i];
            drmModeFreeProperty(p);
            break;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return crtc_id;
}

/* Fold the current connector topology (per-connector connection state, bound CRTC,
 * and that CRTC's active mode) into a hash. Unlike the raw connector/CRTC counts --
 * fixed by the GPU hardware -- this changes when a monitor is plugged/unplugged on an
 * existing connector, a CRTC is (re)bound, or a modeset changes the active resolution
 * or refresh, which is what hotplug/modeset detection needs. GetConnectorCurrent reads
 * cached kernel state (no forced probe). */
static uint64_t topology_hash(int drm_fd, drmModeRes *res) {
    uint64_t h = 1469598103934665603ULL; /* FNV-1a 64-bit offset basis */
#define TH_FOLD(v) do { h ^= (uint64_t)(uint32_t)(v); h *= 1099511628211ULL; } while (0)
    TH_FOLD(res->count_connectors);
    TH_FOLD(res->count_crtcs);
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn =
            drmModeGetConnectorCurrent(drm_fd, res->connectors[i]);
        if (!conn) {
            TH_FOLD(res->connectors[i]);
            continue;
        }
        TH_FOLD(conn->connector_id);
        TH_FOLD(conn->connection);
        uint32_t crtc = connector_crtc_from_atomic(drm_fd, conn->connector_id);
        TH_FOLD(crtc);
        if (crtc) {
            /* Fold the active mode so a modeset (resolution / refresh change) on the
             * same connector+CRTC binding is detected, not just plug/unplug/rebind. */
            drmModeCrtc *c = drmModeGetCrtc(drm_fd, crtc);
            if (c) {
                TH_FOLD(c->mode_valid);
                TH_FOLD(c->mode.hdisplay);
                TH_FOLD(c->mode.vdisplay);
                TH_FOLD(c->mode.vrefresh);
                drmModeFreeCrtc(c);
            }
        }
        drmModeFreeConnector(conn);
    }
#undef TH_FOLD
    return h;
}

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
        /* The legacy encoder link is 0 for compositor-managed / atomically-bound
         * connectors; fall back to the atomic CRTC_ID property so a lit monitor
         * gets its real CRTC instead of enumerating as crtc_id==0 (which
         * open(None,0) mis-handles as "auto-select the first/primary CRTC",
         * silently capturing the wrong monitor). */
        if (crtc_id == 0) {
            crtc_id = connector_crtc_from_atomic(ctx->drm_fd,
                                                 conn->connector_id);
        }

        /* Fill output entry */
        if (found < max_count) {
            drmtap_display *d = &out[found];
            memset(d, 0, sizeof(*d));

            d->connector_id = conn->connector_id;
            d->crtc_id = crtc_id;
            d->active = (crtc_id != 0) ? 1 : 0;

            /* Build connector name: type + type_id (e.g., "HDMI-A-1"), the
             * same shape and spelling /sys/class/drm uses. */
            snprintf(d->name, sizeof(d->name), "%s-%u",
                     drmtap_connector_type_name(conn->connector_type),
                     conn->connector_type_id);

            /* Get current mode and offsets from the CRTC */
            if (crtc_id) {
                drmModeCrtc *crtc = drmModeGetCrtc(ctx->drm_fd, crtc_id);
                if (crtc) {
                    d->x = crtc->x;
                    d->y = crtc->y;
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

    /* Cache the topology signature for hotplug detection. */
    ctx->cached_topology_hash = topology_hash(ctx->drm_fd, res);

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

    /* Hash the per-connector connection + CRTC binding, not the fixed object
     * counts: an ordinary monitor plug/unplug on an existing HDMI/DP connector,
     * and a modeset that rebinds a CRTC, both leave the counts unchanged. */
    uint64_t h = topology_hash(ctx->drm_fd, res);
    int changed = (h != ctx->cached_topology_hash);
    ctx->cached_topology_hash = h;

    drmModeFreeResources(res);
    return changed;
}
