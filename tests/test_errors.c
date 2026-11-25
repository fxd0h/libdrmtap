/*
 * test_errors.c - every error return of the public API leaves a message.
 *
 * Each case runs on a thread of its own (the context-free message is per
 * thread, so it starts empty) and, when it needs a context, on a fresh
 * render-only one (its message starts empty too). A negative return with
 * drmtap_error() still NULL is a failure. Linked against the shared library:
 * this is what a consumer sees.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "drmtap.h"

static int failures = 0;
static int checked = 0;
static const char *g_render;   /* render node for the ctx cases, NULL = skip them */

typedef int (*case_fn)(drmtap_ctx *ctx);
typedef struct { const char *name; case_fn fn; int needs_ctx; int rc; int has_msg; } job_t;

static void *run(void *arg) {
    job_t *j = arg;
    drmtap_ctx *ctx = NULL;
    if (j->needs_ctx) {
        ctx = drmtap_open_render(g_render);
        if (!ctx) {
            j->rc = 1;   /* skipped */
            return NULL;
        }
    }
    j->rc = j->fn(ctx);
    j->has_msg = drmtap_error(ctx) != NULL;
    if (ctx) {
        drmtap_close(ctx);
    }
    return NULL;
}

static void check_case(const char *name, case_fn fn, int needs_ctx) {
    if (needs_ctx && !g_render) {
        return;
    }
    job_t j = { name, fn, needs_ctx, 0, 0 };
    pthread_t t;
    pthread_create(&t, NULL, run, &j);
    pthread_join(t, NULL);
    if (j.rc >= 0) {
        return;   /* not an error return here (or skipped): nothing to check */
    }
    checked++;
    if (!j.has_msg) {
        printf("  FAIL: %s returned %d (%s) and left no message\n", name, j.rc, strerror(-j.rc));
        failures++;
    }
}

/* context-free calls and NULL contexts: the per-thread message */
static drmtap_frame_info F;
static drmtap_dmabuf_desc D;
static drmtap_packet P;
static drmtap_cursor_info C;
static drmtap_display DS[2];
static drmtap_device DV[2];
static uint8_t px[64];
static int n_list_displays(drmtap_ctx *c) { (void)c; return drmtap_list_displays(NULL, DS, 2); }
static int n_displays_changed(drmtap_ctx *c) { (void)c; return drmtap_displays_changed(NULL); }
static int n_grab(drmtap_ctx *c) { (void)c; return drmtap_grab(NULL, &F); }
static int n_grab_desc(drmtap_ctx *c) { (void)c; return drmtap_grab_desc(NULL, &D, &F); }
static int n_grab_mapped(drmtap_ctx *c) { (void)c; return drmtap_grab_mapped(NULL, &F); }
static int n_grab_fast(drmtap_ctx *c) { (void)c; return drmtap_grab_mapped_fast(NULL, &F); }
static int n_convert(drmtap_ctx *c) { (void)c; return drmtap_convert_dmabuf(NULL, &D, &F); }
static int n_grab_packet(drmtap_ctx *c) { (void)c; return drmtap_grab_packet(NULL, &P); }
static int n_convert_packet(drmtap_ctx *c) { (void)c; return drmtap_convert_packet(NULL, px, 8, NULL, 0, &F); }
static int n_cursor(drmtap_ctx *c) { (void)c; return drmtap_get_cursor(NULL, &C); }
static int n_rotation(drmtap_ctx *c) { (void)c; uint32_t r; return drmtap_plane_rotation(NULL, &r); }
static int n_refresh(drmtap_ctx *c) { (void)c; uint64_t a, b; return drmtap_crtc_refresh(NULL, &a, &b); }
static int n_outbuf(drmtap_ctx *c) { (void)c; return drmtap_set_output_buffer(NULL, px, sizeof(px)); }
static int n_drm_fd(drmtap_ctx *c) { (void)c; return drmtap_drm_fd(NULL); }
static int n_list_devices(drmtap_ctx *c) { (void)c; return drmtap_list_devices(DV, 0); }
static int n_deswizzle(drmtap_ctx *c) { (void)c; return drmtap_deswizzle(NULL, px, 4, 4, 16, 16, 0, 64); }
static int n_convert_format(drmtap_ctx *c) { (void)c; return drmtap_convert_format(NULL, px, 4, 4, 16, 16, 0, 0); }
static int n_tonemap(drmtap_ctx *c) { (void)c; return drmtap_tonemap_hdr10(NULL, px, 4, 4, 16, 16, 0, 0); }
static int n_diff(drmtap_ctx *c) { (void)c; drmtap_rect r; return drmtap_diff_frames(NULL, px, 4, 4, 16, &r, 1, 0); }
static int n_hotspot(drmtap_ctx *c) { (void)c; return drmtap_cursor_hotspot_valid(NULL, NULL); }
static int n_open_render(drmtap_ctx *c) { (void)c; return drmtap_open_render("/nonexistent/renderD999") ? 0 : -ENODEV; }
/* a render-only context: the KMS calls refuse it */
static int r_list_displays(drmtap_ctx *c) { return drmtap_list_displays(c, DS, 2); }
static int r_displays_changed(drmtap_ctx *c) { return drmtap_displays_changed(c); }
static int r_grab(drmtap_ctx *c) { return drmtap_grab(c, &F); }
static int r_grab_desc(drmtap_ctx *c) { return drmtap_grab_desc(c, &D, &F); }
static int r_grab_mapped(drmtap_ctx *c) { return drmtap_grab_mapped(c, &F); }
static int r_grab_fast(drmtap_ctx *c) { return drmtap_grab_mapped_fast(c, &F); }
static int r_grab_packet(drmtap_ctx *c) { int r = drmtap_grab_packet(c, &P); drmtap_packet_release(&P); return r; }
static int r_cursor(drmtap_ctx *c) { return drmtap_get_cursor(c, &C); }
static int r_rotation(drmtap_ctx *c) { uint32_t r; return drmtap_plane_rotation(c, &r); }
static int r_refresh(drmtap_ctx *c) { uint64_t a, b; return drmtap_crtc_refresh(c, &a, &b); }
static int r_outbuf_null(drmtap_ctx *c) { return drmtap_set_output_buffer(c, NULL, 16); }
static int r_convert_null(drmtap_ctx *c) { return drmtap_convert_dmabuf(c, NULL, &F); }
static int r_convert_bad(drmtap_ctx *c) { drmtap_dmabuf_desc d; memset(&d, 0, sizeof(d)); d.dma_buf_fd = -1; d.width = 64; d.height = 64; return drmtap_convert_dmabuf(c, &d, &F); }
static int r_convert_packet_bad(drmtap_ctx *c) { memset(px, 0x5a, sizeof(px)); return drmtap_convert_packet(c, px, sizeof(px), NULL, 0, &F); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running error-message tests...\n");
    drmtap_ctx *probe = drmtap_open_render(NULL);
    static char node[64];
    if (probe) {
        const char *n = drmtap_render_node(probe);
        if (n) {
            snprintf(node, sizeof(node), "%s", n);
            g_render = node;
        }
        drmtap_close(probe);
    }
    if (!g_render) {
        printf("  SKIP: no render node, only the context-free cases run\n");
    }
    struct { const char *n; case_fn f; int ctx; } cases[] = {
        {"list_displays(NULL)", n_list_displays, 0}, {"displays_changed(NULL)", n_displays_changed, 0},
        {"grab(NULL)", n_grab, 0}, {"grab_desc(NULL)", n_grab_desc, 0},
        {"grab_mapped(NULL)", n_grab_mapped, 0}, {"grab_mapped_fast(NULL)", n_grab_fast, 0},
        {"convert_dmabuf(NULL)", n_convert, 0}, {"grab_packet(NULL)", n_grab_packet, 0},
        {"convert_packet(NULL)", n_convert_packet, 0}, {"get_cursor(NULL)", n_cursor, 0},
        {"plane_rotation(NULL)", n_rotation, 0}, {"crtc_refresh(NULL)", n_refresh, 0},
        {"set_output_buffer(NULL)", n_outbuf, 0}, {"drm_fd(NULL)", n_drm_fd, 0},
        {"list_devices(max 0)", n_list_devices, 0}, {"deswizzle(NULL src)", n_deswizzle, 0},
        {"convert_format(NULL src)", n_convert_format, 0}, {"tonemap_hdr10(NULL src)", n_tonemap, 0},
        {"diff_frames(NULL a)", n_diff, 0}, {"cursor_hotspot_valid(NULL)", n_hotspot, 0},
        {"open_render(missing node)", n_open_render, 0},
        {"list_displays(render ctx)", r_list_displays, 1}, {"displays_changed(render ctx)", r_displays_changed, 1},
        {"grab(render ctx)", r_grab, 1}, {"grab_desc(render ctx)", r_grab_desc, 1},
        {"grab_mapped(render ctx)", r_grab_mapped, 1}, {"grab_mapped_fast(render ctx)", r_grab_fast, 1},
        {"grab_packet(render ctx)", r_grab_packet, 1}, {"get_cursor(render ctx)", r_cursor, 1},
        {"plane_rotation(render ctx)", r_rotation, 1}, {"crtc_refresh(render ctx)", r_refresh, 1},
        {"set_output_buffer(render ctx, NULL)", r_outbuf_null, 1},
        {"convert_dmabuf(render ctx, NULL desc)", r_convert_null, 1},
        {"convert_dmabuf(render ctx, fd -1)", r_convert_bad, 1},
        {"convert_packet(render ctx, garbage)", r_convert_packet_bad, 1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        check_case(cases[i].n, cases[i].f, cases[i].ctx);
    }
    printf("  %d error returns checked\n", checked);
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("All error returns leave a message\n");
    return 0;
}
