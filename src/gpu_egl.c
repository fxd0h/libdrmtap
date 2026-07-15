/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file gpu_egl.c
 * @brief EGL/OpenGL ES backend — GPU-universal tiled→linear conversion
 *
 * This backend uses EGL to import a DMA-BUF framebuffer as an EGLImage,
 * then renders it to a linear RGBA texture via OpenGL ES. The GPU driver
 * handles its own tiling format transparently during the texture read.
 *
 * This works for ALL GPU vendors (Intel, AMD, Nvidia, RPi) with a single
 * code path, because EGL_EXT_image_dma_buf_import with modifiers delegates
 * format interpretation to the driver.
 *
 * Approach based on ReFrame (AlynxZhou/reframe), validated in production
 * across Intel, AMD, Nvidia, and Raspberry Pi.
 *
 * Required extensions:
 *   - EGL_EXT_device_query
 *   - EGL_EXT_platform_device
 *   - EGL_EXT_image_dma_buf_import
 *   - EGL_EXT_image_dma_buf_import_modifiers
 *   - GL_OES_EGL_image_external
 */

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#ifdef HAVE_EGL

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <libdrm/drm_fourcc.h>
#include "drmtap_internal.h"

/* ========================================================================= */
/* EGL context (lazily initialized per drmtap_ctx)                           */
/* ========================================================================= */

/* Global EGL display (one per process, shared across threads) */
static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static int g_egl_display_initialized = 0;

/* Per-thread EGL context and GL resources */
typedef struct {
    EGLDisplay display;   /* cached copy of g_egl_display */
    EGLContext context;
    GLuint program;
    GLuint fbo;
    GLuint linear_texture;
    uint32_t tex_width;
    uint32_t tex_height;
    int initialized;
    pthread_t owner_thread;  /* thread that created this context */
} egl_state_t;

/* The per-thread EGL context + GL resources.
 *
 * MUST be thread-local: an EGL/GL context is current in at most one thread, and
 * each capture (e.g. one per monitor in a multi-display session) runs on its own
 * thread. A single process-global state shared across threads would race on the
 * FBO/program/textures, producing corrupted/garbage frames under concurrent
 * capture. Thread-local storage gives each capture thread its own EGL context
 * over the shared EGLDisplay — the correct EGL threading model.
 *
 * It lives at FILE scope (not inside egl_convert_impl) so drmtap_gpu_egl_thread_cleanup()
 * can release it on drmtap_close(): C thread-local storage has no destructor, so
 * without an explicit teardown every open/close cycle on a fresh capture thread
 * leaked a whole EGL context + a w*h*4 linear texture (~33 MB at 4K). */
static _Thread_local egl_state_t state = {0};

/* Backstop for a capture thread that exits WITHOUT calling drmtap_close (an
 * error/panic path): a pthread TSD destructor frees this thread's EGL context at
 * thread exit, while its context can still be made current. The shared
 * g_egl_display is never terminated, so this is safe. */
static pthread_key_t g_egl_tsd_key;
static pthread_once_t g_egl_tsd_once = PTHREAD_ONCE_INIT;
static void egl_cleanup(egl_state_t *st);
static void egl_tsd_destructor(void *unused) {
    (void)unused;
    egl_cleanup(&state);
}
static void egl_tsd_key_init(void) {
    pthread_key_create(&g_egl_tsd_key, egl_tsd_destructor);
}

/* EGL function pointers (loaded dynamically) */
static PFNEGLQUERYDEVICESEXTPROC pfn_eglQueryDevicesEXT;
static PFNEGLQUERYDEVICESTRINGEXTPROC pfn_eglQueryDeviceStringEXT;
static PFNEGLGETPLATFORMDISPLAYEXTPROC pfn_eglGetPlatformDisplayEXT;
static PFNEGLCREATEIMAGEKHRPROC pfn_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC pfn_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pfn_glEGLImageTargetTexture2DOES;

static int load_egl_procs(void) {
    pfn_eglQueryDevicesEXT = (PFNEGLQUERYDEVICESEXTPROC)
        eglGetProcAddress("eglQueryDevicesEXT");
    pfn_eglQueryDeviceStringEXT = (PFNEGLQUERYDEVICESTRINGEXTPROC)
        eglGetProcAddress("eglQueryDeviceStringEXT");
    pfn_eglGetPlatformDisplayEXT = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
    pfn_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    pfn_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    pfn_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");

    return (pfn_eglCreateImageKHR && pfn_eglDestroyImageKHR &&
            pfn_glEGLImageTargetTexture2DOES) ? 0 : -1;
}

/* ========================================================================= */
/* Shader source                                                             */
/* ========================================================================= */

static const char *vertex_shader_src =
    "#version 100\n"
    "attribute vec2 in_position;\n"
    "attribute vec2 in_texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(in_position, 0.0, 1.0);\n"
    "    v_texcoord = in_texcoord;\n"
    "}\n";

/* The fragment shader samples the external (DRM) texture and outputs BGRA. When
 * the scanout is HDR10 (u_hdr == 1) it additionally tone-maps the PQ/BT.2020
 * sample to SDR/BT.709 8-bit — the same pipeline as the CPU path in
 * pixel_convert.c (PQ EOTF -> gamut -> highlight soft-knee -> sRGB), done on the
 * GPU so tiled HDR scanouts (which can't be CPU-deswizzled) are handled too. */
static const char *fragment_shader_src =
    "#version 100\n"
    "#extension GL_OES_EGL_image_external : require\n"
    "precision highp float;\n"
    "uniform samplerExternalOES image;\n"
    "uniform int u_hdr;\n"
    "uniform float u_peak_n;\n"
    "varying vec2 v_texcoord;\n"
    "float pq_eotf(float e) {\n"
    "    float m1 = 0.1593017578125;\n"
    "    float m2 = 78.84375;\n"
    "    float c1 = 0.8359375;\n"
    "    float c2 = 18.8515625;\n"
    "    float c3 = 18.6875;\n"
    "    float ep = pow(max(e, 0.0), 1.0 / m2);\n"
    "    float num = max(ep - c1, 0.0);\n"
    "    float den = max(c2 - c3 * ep, 1e-6);\n"
    "    return 10000.0 * pow(num / den, 1.0 / m1);\n"
    "}\n"
    "float knee_curve(float x, float p) {\n"
    "    float knee = 0.9;\n"
    "    if (x <= knee) return x;\n"
    "    float pk = max(p, knee + 0.5);\n"
    "    if (x >= pk) return 1.0;\n"
    "    float t = (x - knee) / (pk - knee);\n"
    "    return knee + (1.0 - knee) * (t * (2.0 - t));\n"
    "}\n"
    "float srgb_oetf(float c) {\n"
    "    c = clamp(c, 0.0, 1.0);\n"
    "    if (c <= 0.0031308) return 12.92 * c;\n"
    "    return 1.055 * pow(c, 1.0 / 2.4) - 0.055;\n"
    "}\n"
    "void main() {\n"
    "    vec4 c = texture2D(image, v_texcoord);\n"
    "    if (u_hdr == 1) {\n"
    "        float R = pq_eotf(c.r);\n"
    "        float G = pq_eotf(c.g);\n"
    "        float B = pq_eotf(c.b);\n"
    "        float r =  1.660491 * R - 0.587641 * G - 0.072850 * B;\n"
    "        float g = -0.124550 * R + 1.132900 * G - 0.008349 * B;\n"
    "        float b = -0.018151 * R - 0.100579 * G + 1.118730 * B;\n"
    "        float w = 203.0;\n"
    "        r = srgb_oetf(knee_curve(max(r, 0.0) / w, u_peak_n));\n"
    "        g = srgb_oetf(knee_curve(max(g, 0.0) / w, u_peak_n));\n"
    "        b = srgb_oetf(knee_curve(max(b, 0.0) / w, u_peak_n));\n"
    "        gl_FragColor = vec4(b, g, r, c.a);\n"
    "    } else {\n"
    "        gl_FragColor = vec4(c.b, c.g, c.r, c.a);\n"
    "    }\n"
    "}\n";

/* ========================================================================= */
/* GL helpers                                                                */
/* ========================================================================= */

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    if (shader == 0) {
        return 0;
    }
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint create_program(void) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_src);
    if (vs == 0 || fs == 0) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "in_position");
    glBindAttribLocation(program, 1, "in_texcoord");
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

/* ========================================================================= */
/* EGL display from DRM card path                                            */
/* ========================================================================= */

static EGLDisplay get_egl_display_for_card(const char *card_path) {
    if (!pfn_eglQueryDevicesEXT || !pfn_eglQueryDeviceStringEXT ||
        !pfn_eglGetPlatformDisplayEXT) {
        return eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }

    EGLint max_devices = 0;
    pfn_eglQueryDevicesEXT(0, NULL, &max_devices);
    if (max_devices <= 0) {
        return eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }

    EGLDeviceEXT *devices = calloc(max_devices, sizeof(EGLDeviceEXT));
    if (!devices) {
        return eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }

    EGLint num_devices = 0;
    pfn_eglQueryDevicesEXT(max_devices, devices, &num_devices);

    EGLDisplay display = EGL_NO_DISPLAY;
    for (int i = 0; i < num_devices; i++) {
        const char *drm_path = pfn_eglQueryDeviceStringEXT(
            devices[i], EGL_DRM_DEVICE_FILE_EXT);
        if (drm_path && strcmp(drm_path, card_path) == 0) {
            display = pfn_eglGetPlatformDisplayEXT(
                EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);
            break;
        }
    }
    free(devices);

    if (display == EGL_NO_DISPLAY) {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    return display;
}

/* ========================================================================= */
/* EGL state management                                                      */
/* ========================================================================= */

static int egl_init(drmtap_ctx *ctx, egl_state_t *state) {
    if (load_egl_procs() < 0) {
        drmtap_debug_log(ctx, "egl: required EGL procs not available");
        return -ENOTSUP;
    }

    /* Initialize global display once (thread-safe: eglInitialize is 
     * idempotent per EGL spec — second call on same display is a no-op) */
    if (!g_egl_display_initialized) {
        g_egl_display = get_egl_display_for_card(ctx->device_path);
        if (g_egl_display == EGL_NO_DISPLAY) {
            drmtap_debug_log(ctx, "egl: no EGL display available");
            return -ENODEV;
        }
        EGLint major, minor;
        if (!eglInitialize(g_egl_display, &major, &minor)) {
            drmtap_debug_log(ctx, "egl: eglInitialize failed: 0x%x",
                             eglGetError());
            return -EIO;
        }
        g_egl_display_initialized = 1;
        drmtap_debug_log(NULL, "EGL: global display initialized: %p (%d.%d)",
                (void*)g_egl_display, major, minor);
    }
    state->display = g_egl_display;

    eglBindAPI(EGL_OPENGL_ES_API);

    /* Create context */
    EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    /* Re-initialize for this thread (no-op if same thread, required if different) */
    EGLint reinit_maj, reinit_min;
    if (!eglInitialize(state->display, &reinit_maj, &reinit_min)) {
        drmtap_debug_log(NULL, "EGL: eglInitialize for thread FAILED: 0x%x", eglGetError());
        return -EIO;
    }
    drmtap_debug_log(NULL, "EGL: eglInitialize OK (%d.%d)", reinit_maj, reinit_min);
    drmtap_debug_log(NULL, "EGL: eglChooseConfig...");
    EGLConfig config;
    EGLint num_configs = 0;
    if (!eglChooseConfig(state->display, config_attribs, &config, 1,
                         &num_configs) || num_configs == 0) {
        drmtap_debug_log(NULL, "EGL: eglChooseConfig FAILED num=%d err=0x%x", num_configs, eglGetError());
        drmtap_debug_log(ctx, "egl: eglChooseConfig failed or no configs");
        return -EIO;
    }

    EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    drmtap_debug_log(NULL, "EGL: eglCreateContext(config=%p)...", (void*)config);
    state->context = eglCreateContext(state->display, config,
                                      EGL_NO_CONTEXT, context_attribs);
    if (state->context == EGL_NO_CONTEXT) {
        drmtap_debug_log(NULL, "EGL: eglCreateContext FAILED err=0x%x", eglGetError());
        drmtap_debug_log(ctx, "egl: eglCreateContext failed: 0x%x",
                         eglGetError());
        return -EIO;
    }

    eglMakeCurrent(state->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   state->context);

    /* Create shader program */
    state->program = create_program();
    if (state->program == 0) {
        drmtap_debug_log(NULL, "EGL: shader compilation FAILED");
        drmtap_debug_log(ctx, "egl: shader compilation failed");
        eglDestroyContext(state->display, state->context);
        return -EIO;
    }

    /* Create FBO for offscreen rendering */
    glGenFramebuffers(1, &state->fbo);

    state->linear_texture = 0;
    state->tex_width = 0;
    state->tex_height = 0;
    state->initialized = 1;
    state->owner_thread = pthread_self();
    /* Register the thread-exit backstop so a capture thread that dies without
     * drmtap_close() still frees this context (value must be non-NULL to arm
     * the destructor; the destructor frees the thread-local `state` directly). */
    pthread_once(&g_egl_tsd_once, egl_tsd_key_init);
    pthread_setspecific(g_egl_tsd_key, state);
    drmtap_debug_log(NULL, "EGL: init OK: display=%p ctx=%p program=%u fbo=%u (tid=%ld)",
            (void*)state->display, (void*)state->context, state->program, state->fbo, (long)(long)0);

    drmtap_debug_log(ctx, "egl: GPU-universal backend initialized");
    return 0;
}

static void egl_cleanup(egl_state_t *st) {
    if (!st->initialized) {
        return;
    }
    eglMakeCurrent(st->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   st->context);
    if (st->linear_texture) {
        glDeleteTextures(1, &st->linear_texture);
    }
    if (st->fbo) {
        glDeleteFramebuffers(1, &st->fbo);
    }
    if (st->program) {
        glDeleteProgram(st->program);
    }
    /* Release the context from this thread before destroying it. MUST NOT
     * eglTerminate(st->display): g_egl_display is shared across all capture
     * threads; terminating it invalidates other threads' live detile contexts
     * (see the CRITICAL note in drmtap_gpu_egl_available). */
    eglMakeCurrent(st->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    eglDestroyContext(st->display, st->context);
    st->context = EGL_NO_CONTEXT;
    st->program = 0;
    st->fbo = 0;
    st->linear_texture = 0;
    st->tex_width = 0;
    st->tex_height = 0;
    st->initialized = 0;
}

/* Release THIS thread's EGL context + GL resources. Safe no-op if this thread
 * never built a context. Called from drmtap_close() on the capture thread (and
 * by the TSD backstop at thread exit); idempotent. */
void drmtap_gpu_egl_thread_cleanup(void) {
    egl_cleanup(&state);
}

/* Ensure the linear output texture matches the capture dimensions */
static void ensure_linear_texture(egl_state_t *state, uint32_t w, uint32_t h) {
    if (state->tex_width == w && state->tex_height == h) {
        return;
    }
    if (state->linear_texture) {
        glDeleteTextures(1, &state->linear_texture);
    }
    glGenTextures(1, &state->linear_texture);
    glBindTexture(GL_TEXTURE_2D, state->linear_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    state->tex_width = w;
    state->tex_height = h;
}

/* ========================================================================= */
/* Public API                                                                */
/* ========================================================================= */

/**
 * Check if EGL detiling is available on this system.
 * Returns 1 if EGL + GLES are functional, 0 otherwise.
 */
int drmtap_gpu_egl_available(drmtap_ctx *ctx) {
    /* EGL availability is a static property of the GPU, but this is called on
     * every captured frame. Cache it once.
     *
     * CRITICAL: this must NOT call eglTerminate() on the display. The display
     * returned by get_egl_display_for_card() is the SAME shared EGLDisplay used
     * by the live detile contexts. Terminating it here — while another capture
     * thread is mid-eglCreateImage — invalidates that thread's display and makes
     * its detile fail with EGL_NOT_INITIALIZED, so it falls back to returning the
     * raw tiled/compressed buffer (garbage/color corruption). Concurrent captures
     * (e.g. two monitors viewed at once) hit this constantly. */
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    static int cached = -1;
    pthread_mutex_lock(&lk);
    if (cached < 0) {
        if (load_egl_procs() < 0) {
            cached = 0;
        } else {
            EGLDisplay display = get_egl_display_for_card(ctx->device_path);
            if (display == EGL_NO_DISPLAY) {
                cached = 0;
            } else {
                EGLint major, minor;
                cached = eglInitialize(display, &major, &minor) ? 1 : 0;
                /* Intentionally no eglTerminate(display) — see above. */
            }
        }
    }
    pthread_mutex_unlock(&lk);
    return cached;
}

/**
 * Convert a tiled DMA-BUF framebuffer to linear RGBA pixels using EGL/GL.
 *
 * This is the universal GPU detiling path. The GPU driver handles its
 * own tiling format transparently when the DMA-BUF is imported as an
 * EGLImage with the correct modifier.
 *
 * @param ctx       Capture context
 * @param dma_buf_fd  DMA-BUF file descriptor
 * @param width     Framebuffer width
 * @param height    Framebuffer height
 * @param stride    Framebuffer stride (bytes per row)
 * @param fourcc    DRM fourcc format (e.g., DRM_FORMAT_XRGB8888)
 * @param modifier  DRM framebuffer modifier (tiling info)
 * @param out_data  Output: allocated linear RGBA buffer (caller must free)
 * @param out_size  Output: size of the allocated buffer
 * @return 0 on success, negative errno on error
 */
static int egl_convert_impl(drmtap_ctx *ctx,
                            int dma_buf_fd,
                            uint32_t width, uint32_t height,
                            uint32_t stride, uint32_t fourcc,
                            uint64_t modifier,
                            void **out_data, size_t *out_size);

/* Public entry point: serialize GPU detiles across the whole process.
 *
 * Each capture runs on its own thread with its own thread-local EGL context, but
 * the GPU is the real bottleneck. When two 4K monitors are captured at once,
 * letting both detiles run concurrently makes each take longer (wall-clock) than
 * the compositor's page-flip interval, so the live scanout buffer is recycled
 * mid-read and the CCS-compressed result comes out as garbage/color corruption.
 * A single process-global lock keeps every detile short enough to complete within
 * one flip interval, which eliminates the corruption while each thread still owns
 * its EGL context. */
int drmtap_gpu_egl_convert(drmtap_ctx *ctx,
                           int dma_buf_fd,
                           uint32_t width, uint32_t height,
                           uint32_t stride, uint32_t fourcc,
                           uint64_t modifier,
                           void **out_data, size_t *out_size) {
    static pthread_mutex_t g_convert_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&g_convert_lock);
    int _ret = egl_convert_impl(ctx, dma_buf_fd, width, height, stride, fourcc,
                                modifier, out_data, out_size);
    pthread_mutex_unlock(&g_convert_lock);
    return _ret;
}

static int egl_convert_impl(drmtap_ctx *ctx,
                            int dma_buf_fd,
                            uint32_t width, uint32_t height,
                            uint32_t stride, uint32_t fourcc,
                            uint64_t modifier,
                            void **out_data, size_t *out_size) {
    if (!out_data || !out_size) {
        return -EINVAL;
    }

    /* Lazy-init this thread's EGL context. `state` is the file-scope
     * thread-local defined above; it is freed by drmtap_gpu_egl_thread_cleanup()
     * on close. (A `_Thread_local` is per-thread, so there is no cross-thread
     * owner check to do here — each thread only ever sees its own instance.) */
    int ret;

    drmtap_debug_log(NULL, "EGL: state.initialized=%d", state.initialized);
    if (!state.initialized) {
        ret = egl_init(ctx, &state);
        drmtap_debug_log(NULL, "EGL: egl_init returned %d", ret);
        if (ret < 0) {
            return ret;
        }
    }

    /* Ensure display is initialized for this thread and context is current */
    eglBindAPI(EGL_OPENGL_ES_API);
    {
        EGLint _maj, _min;
        eglInitialize(state.display, &_maj, &_min);  /* no-op if already init */
    }
    if (!eglMakeCurrent(state.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        state.context)) {
        drmtap_debug_log(NULL, "EGL: eglMakeCurrent in convert failed: 0x%x", eglGetError());
    }

    /* Build EGLImage attributes with DMA-BUF import */
    EGLint image_attribs[64];
    int ai = 0;
    image_attribs[ai++] = EGL_WIDTH;
    image_attribs[ai++] = (EGLint)width;
    image_attribs[ai++] = EGL_HEIGHT;
    image_attribs[ai++] = (EGLint)height;
    image_attribs[ai++] = EGL_LINUX_DRM_FOURCC_EXT;
    image_attribs[ai++] = (EGLint)fourcc;

    /* Plane 0: main surface */
    image_attribs[ai++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    image_attribs[ai++] = dma_buf_fd;
    image_attribs[ai++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    image_attribs[ai++] = (EGLint)ctx->fb2_offsets[0];
    image_attribs[ai++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    image_attribs[ai++] = (EGLint)stride;

    /* Include modifier if valid (not DRM_FORMAT_MOD_INVALID) */
    if (modifier != DRM_FORMAT_MOD_INVALID) {
        image_attribs[ai++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        image_attribs[ai++] = (EGLint)(modifier & 0xFFFFFFFF);
        image_attribs[ai++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        image_attribs[ai++] = (EGLint)(modifier >> 32);
    }

    /* Plane 1: CCS auxiliary surface (Gen12+ CCS) */
    if (ctx->fb2_num_planes >= 2 && modifier != DRM_FORMAT_MOD_INVALID) {
        drmtap_debug_log(ctx, "egl: adding CCS aux plane1 "
            "(offset=%u pitch=%u)",
            ctx->fb2_offsets[1], ctx->fb2_pitches[1]);
        image_attribs[ai++] = EGL_DMA_BUF_PLANE1_FD_EXT;
        image_attribs[ai++] = dma_buf_fd;
        image_attribs[ai++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
        image_attribs[ai++] = (EGLint)ctx->fb2_offsets[1];
        image_attribs[ai++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
        image_attribs[ai++] = (EGLint)ctx->fb2_pitches[1];
        image_attribs[ai++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
        image_attribs[ai++] = (EGLint)(modifier & 0xFFFFFFFF);
        image_attribs[ai++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
        image_attribs[ai++] = (EGLint)(modifier >> 32);
    }

    /* Plane 2: Clear Color metadata (Gen12+ CCS_CC) */
    if (ctx->fb2_num_planes >= 3 && modifier != DRM_FORMAT_MOD_INVALID) {
        drmtap_debug_log(ctx, "egl: adding CC plane2 "
            "(offset=%u pitch=%u)",
            ctx->fb2_offsets[2], ctx->fb2_pitches[2]);
        image_attribs[ai++] = EGL_DMA_BUF_PLANE2_FD_EXT;
        image_attribs[ai++] = dma_buf_fd;
        image_attribs[ai++] = EGL_DMA_BUF_PLANE2_OFFSET_EXT;
        image_attribs[ai++] = (EGLint)ctx->fb2_offsets[2];
        image_attribs[ai++] = EGL_DMA_BUF_PLANE2_PITCH_EXT;
        image_attribs[ai++] = (EGLint)ctx->fb2_pitches[2];
        image_attribs[ai++] = EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT;
        image_attribs[ai++] = (EGLint)(modifier & 0xFFFFFFFF);
        image_attribs[ai++] = EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT;
        image_attribs[ai++] = (EGLint)(modifier >> 32);
    }

    image_attribs[ai++] = EGL_NONE;

    drmtap_debug_log(NULL, "EGL: tid=%lu creating EGLImage: fd=%d %ux%u stride=%u fourcc=0x%x mod=0x%lx ai=%d",
            (unsigned long)pthread_self(), dma_buf_fd, width, height, stride, fourcc, (unsigned long)modifier, ai);

    /* eglMakeCurrent already done at function entry */

    /* Import DMA-BUF as EGLImage */
    EGLImageKHR image = pfn_eglCreateImageKHR(
        state.display, EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT, NULL, image_attribs);
    if (image == EGL_NO_IMAGE_KHR) {
        EGLint first_err = eglGetError();
        drmtap_debug_log(NULL, "EGL: eglCreateImage FAILED: err=0x%x fourcc=0x%x mod=0x%lx",
                first_err, fourcc, (unsigned long)modifier);
        drmtap_debug_log(ctx, "egl: eglCreateImage failed: 0x%x "
                         "(fourcc=%.4s modifier=0x%lx)",
                         first_err, (const char *)&fourcc,
                         (unsigned long)modifier);

        /* Retry with XRGB8888 but keep modifier — the driver needs
         * the modifier for detiling but may support 8-bit import */
        uint32_t xrgb8888 = DRM_FORMAT_XRGB8888;
        EGLint retry_attribs[32];
        int ri = 0;
        retry_attribs[ri++] = EGL_WIDTH;
        retry_attribs[ri++] = (EGLint)width;
        retry_attribs[ri++] = EGL_HEIGHT;
        retry_attribs[ri++] = (EGLint)height;
        retry_attribs[ri++] = EGL_LINUX_DRM_FOURCC_EXT;
        retry_attribs[ri++] = (EGLint)xrgb8888;
        retry_attribs[ri++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        retry_attribs[ri++] = dma_buf_fd;
        retry_attribs[ri++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        retry_attribs[ri++] = 0;
        retry_attribs[ri++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        retry_attribs[ri++] = (EGLint)stride;
        if (modifier != DRM_FORMAT_MOD_INVALID) {
            retry_attribs[ri++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            retry_attribs[ri++] = (EGLint)(modifier & 0xFFFFFFFF);
            retry_attribs[ri++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            retry_attribs[ri++] = (EGLint)(modifier >> 32);
        }
        retry_attribs[ri++] = EGL_NONE;

        drmtap_debug_log(ctx, "egl: retrying with XRGB8888 + modifier");
        image = pfn_eglCreateImageKHR(
            state.display, EGL_NO_CONTEXT,
            EGL_LINUX_DMA_BUF_EXT, NULL, retry_attribs);

        if (image == EGL_NO_IMAGE_KHR) {
            /* Last resort: try XRGB8888 without modifier */
            ri = 0;
            retry_attribs[ri++] = EGL_WIDTH;
            retry_attribs[ri++] = (EGLint)width;
            retry_attribs[ri++] = EGL_HEIGHT;
            retry_attribs[ri++] = (EGLint)height;
            retry_attribs[ri++] = EGL_LINUX_DRM_FOURCC_EXT;
            retry_attribs[ri++] = (EGLint)xrgb8888;
            retry_attribs[ri++] = EGL_DMA_BUF_PLANE0_FD_EXT;
            retry_attribs[ri++] = dma_buf_fd;
            retry_attribs[ri++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
            retry_attribs[ri++] = 0;
            retry_attribs[ri++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
            retry_attribs[ri++] = (EGLint)stride;
            retry_attribs[ri++] = EGL_NONE;

            drmtap_debug_log(ctx, "egl: retrying XRGB8888 no-modifier");
            image = pfn_eglCreateImageKHR(
                state.display, EGL_NO_CONTEXT,
                EGL_LINUX_DMA_BUF_EXT, NULL, retry_attribs);
        }

        if (image == EGL_NO_IMAGE_KHR) {
            drmtap_debug_log(NULL, "EGL: retry XRGB8888+nomod also FAILED: err=0x%x", eglGetError());
            drmtap_debug_log(ctx, "egl: all retries failed: 0x%x",
                             eglGetError());
            return -EIO;
        }
        drmtap_debug_log(NULL, "EGL: retry SUCCEEDED");
        drmtap_debug_log(ctx, "egl: retry succeeded");
    }

    /* Bind as GL_TEXTURE_EXTERNAL_OES — GPU handles detiling */
    GLuint ext_texture;
    glGenTextures(1, &ext_texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ext_texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S,
                    GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T,
                    GL_CLAMP_TO_EDGE);
    pfn_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

    GLenum gl_err = glGetError();
    drmtap_debug_log(NULL, "EGL: glEGLImageTargetTexture2DOES: gl_err=0x%x", gl_err);
    if (gl_err != GL_NO_ERROR) {
        drmtap_debug_log(ctx, "egl: glEGLImageTargetTexture2DOES failed: 0x%x",
                         gl_err);
        glDeleteTextures(1, &ext_texture);
        pfn_eglDestroyImageKHR(state.display, image);
        return -EIO;
    }

    /* Ensure output texture */
    ensure_linear_texture(&state, width, height);

    /* Setup FBO with linear output texture */
    glBindFramebuffer(GL_FRAMEBUFFER, state.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, state.linear_texture, 0);
    glViewport(0, 0, width, height);

    /* Render fullscreen quad: external texture → linear FBO */
    glUseProgram(state.program);
    glUniform1i(glGetUniformLocation(state.program, "image"), 0);

    /* HDR10 scanout: tone-map PQ/BT.2020 -> SDR in the shader. eotf/peak come
     * from the connector HDR_OUTPUT_METADATA (carried on ctx). */
    int hdr_on = (ctx && ctx->cur_hdr_eotf == DRMTAP_EOTF_PQ) ? 1 : 0;
    glUniform1i(glGetUniformLocation(state.program, "u_hdr"), hdr_on);
    {
        uint32_t mn = (ctx && ctx->cur_hdr_max_nits > 0)
                          ? ctx->cur_hdr_max_nits : 1000u;
        glUniform1f(glGetUniformLocation(state.program, "u_peak_n"),
                    (float)((double)mn / 203.0));
    }
    if (hdr_on) {
        drmtap_debug_log(ctx, "EGL: HDR tone-map in shader (peak=%u nits)",
                         ctx->cur_hdr_max_nits);
    }

    /* Fullscreen triangle strip.
     * Use standard unflipped coordinates. The EGL texture is natively
     * horizontally mirrored on Gen12 CCS. We fix this by flipping the
     * X coordinate in the fragment shader (1.0 - v_texcoord.x) to avoid
     * any vertex winding or Culling anomalies. */
    static const float vertices[] = {
        /* position    texcoord (unflipped) */
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
    };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          vertices);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          vertices + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);

    /* Ensure GPU rendering is complete before reading back */
    glFinish();

    /* Read linear pixels back */
    size_t rgba_size = (size_t)width * height * 4;
    void *pixels = malloc(rgba_size);
    if (!pixels) {
        ret = -ENOMEM;
        goto cleanup;
    }
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    drmtap_debug_log(NULL, "EGL: glReadPixels done, gl_err=0x%x", glGetError());

    /* NOTE: CPU horizontal flip removed. The EGL DMA-BUF import with
     * standard texcoords (no shader manipulation) produces correct
     * orientation. The previous CPU flip was compensating for a shader-
     * based X flip (1.0 - v_texcoord.x) that has since been removed. */

    *out_data = pixels;
    *out_size = rgba_size;
    ret = 0;

    drmtap_debug_log(ctx, "egl: converted %ux%u tiled→linear (%zu bytes)",
                     width, height, rgba_size);

cleanup:
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    glDeleteTextures(1, &ext_texture);
    pfn_eglDestroyImageKHR(state.display, image);

    /* The context stays current on this (capture) thread and is reused across
     * calls — each thread owns its own thread-local context. It is released by
     * drmtap_gpu_egl_thread_cleanup() at drmtap_close() / thread exit. */
    return ret;
}

#else /* !HAVE_EGL */

#include "drmtap_internal.h"

int drmtap_gpu_egl_available(drmtap_ctx *ctx) {
    (void)ctx;
    return 0;
}

int drmtap_gpu_egl_convert(drmtap_ctx *ctx,
                            int dma_buf_fd,
                            uint32_t width, uint32_t height,
                            uint32_t stride, uint32_t fourcc,
                            uint64_t modifier,
                            void **out_data, size_t *out_size) {
    (void)ctx; (void)dma_buf_fd; (void)width; (void)height;
    (void)stride; (void)fourcc; (void)modifier;
    (void)out_data; (void)out_size;
    drmtap_set_error(ctx, "EGL backend not available (built without EGL)");
    return -ENOTSUP;
}

void drmtap_gpu_egl_thread_cleanup(void) {
    /* No EGL backend compiled in — nothing to release. */
}

#endif /* HAVE_EGL */
