/*
 * test_planecap.c - a device with more planes than the plane cache holds
 * still captures (integration: needs a KMS device and CAP_SYS_ADMIN).
 *
 * The plane cache holds DRMTAP_KMS_MAX_PLANES non-cursor planes. A device
 * with more must not lose its primary plane (0 lit -> -ENODEV, where 0.5.x
 * captured): the cache gives up and the capture takes the legacy
 * single-plane lookup. Simulated by building the cache with a lower limit.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "drmtap.h"
#include "drmtap_internal.h"
#include "layers_kms.h"

static int failures = 0;
static void check(int cond, const char *what) {
    printf("  %s: %s\n", cond ? "OK" : "FAIL", what);
    if (!cond) {
        failures++;
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running plane-cache limit test...\n");
    drmtap_ctx *ctx = drmtap_open(NULL);
    if (!ctx) {
        printf("  SKIP: no capture device (%s)\n", drmtap_error(NULL) ? drmtap_error(NULL) : "-");
        return 0;
    }
    int fd = drmtap_drm_fd(ctx);
    drmtap_kms_cache full;
    int r = drmtap_kms_cache_build(fd, &full);
    if (r != 0) {
        printf("  SKIP: no atomic plane properties here (%d)\n", r);
        drmtap_close(ctx);
        return 0;
    }
    int cursors = 0;
    for (uint32_t i = 0; i < full.count; i++) {
        cursors += full.planes[i].type == DRM_PLANE_TYPE_CURSOR;
    }
    printf("  %u non-cursor planes cached\n", full.count);
    check(cursors == 0 && full.count >= 1, "the cache holds the non-cursor planes only");
    drmtap_kms_cache small;
    r = drmtap_kms_cache_build_max(fd, &small, full.count - 1);
    check(r == -ENOTSUP && small.state < 0 && small.count == 0,
          "one plane more than the limit: the cache gives up (-ENOTSUP)");
    /* The capture with that verdict: the legacy primary lookup, as 0.5.x. */
    drmtap_frame_info f;
    int g0 = drmtap_grab_mapped(ctx, &f);
    if (g0 == 0) {
        drmtap_frame_release(ctx, &f);
    }
    ctx->layer_cache = small;
    int g = drmtap_grab_mapped(ctx, &f);
    if (g == 0) {
        drmtap_frame_release(ctx, &f);
    }
    if (g0 != 0) {
        printf("  SKIP: no capture here at all (%d): %s\n", g0, drmtap_error(ctx));
    } else {
        check(g == 0, "and the capture still works, through the legacy lookup");
    }
    drmtap_close(ctx);
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("All plane-cache tests passed!\n");
    return 0;
}
