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

/* _GNU_SOURCE: expose POSIX types (ino_t via <sys/stat.h>) and syscall() under
 * -std=c11 strict mode, matching the other translation units. Guarded because
 * the Rust cc build passes -D_GNU_SOURCE on the command line. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#ifdef HAVE_EGL

#include <dlfcn.h>
#include <sys/stat.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <libdrm/drm_fourcc.h>
#include "drmtap_internal.h"

/* ========================================================================= */
/* Lazy runtime loading of libEGL / libGLESv2                                */
/* ========================================================================= */
/*
 * libEGL and libGLESv2 are resolved with dlopen() on first use instead of
 * being linked as DT_NEEDED, so the GPU userspace stack (and everything it
 * drags in) is only mapped into processes that actually convert frames. In
 * the split capture model the privileged exporter calls only
 * drmtap_open()/drmtap_grab(), which never reach this backend, so the GL
 * stack never loads into the privileged process.
 *
 * The handles are never dlclose()d: GL drivers install thread-local state and
 * worker threads that do not survive an unload, and the per-thread contexts
 * released by the TSD destructor can still run at thread exit. One-time load,
 * process lifetime — the same lifetime a DT_NEEDED link had.
 */

#define DRMTAP_EGL_CORE_SYMBOLS(X) \
    X(eglBindAPI) \
    X(eglChooseConfig) \
    X(eglCreateContext) \
    X(eglDestroyContext) \
    X(eglGetDisplay) \
    X(eglGetError) \
    X(eglGetProcAddress) \
    X(eglInitialize) \
    X(eglMakeCurrent)

#define DRMTAP_GLES_CORE_SYMBOLS(X) \
    X(glActiveTexture) \
    X(glAttachShader) \
    X(glBindAttribLocation) \
    X(glBindFramebuffer) \
    X(glBindTexture) \
    X(glCompileShader) \
    X(glCreateProgram) \
    X(glCreateShader) \
    X(glDeleteFramebuffers) \
    X(glDeleteProgram) \
    X(glDeleteShader) \
    X(glDeleteTextures) \
    X(glDisableVertexAttribArray) \
    X(glDrawArrays) \
    X(glEnableVertexAttribArray) \
    X(glFinish) \
    X(glFramebufferTexture2D) \
    X(glGenFramebuffers) \
    X(glGenTextures) \
    X(glGetError) \
    X(glGetProgramiv) \
    X(glGetShaderiv) \
    X(glGetUniformLocation) \
    X(glLinkProgram) \
    X(glReadPixels) \
    X(glShaderSource) \
    X(glTexImage2D) \
    X(glTexParameteri) \
    X(glUniform1f) \
    X(glUniform1i) \
    X(glUseProgram) \
    X(glVertexAttribPointer) \
    X(glViewport)

/* One function pointer per symbol, typed from the real prototype in the EGL/
 * GLES headers so a signature drift is a compile error. Declared BEFORE the
 * name-routing defines below so __typeof__ sees the header declaration. */
#define DRMTAP_DECLARE_GL_PFN(name) static __typeof__(name) *pfn_##name;
DRMTAP_EGL_CORE_SYMBOLS(DRMTAP_DECLARE_GL_PFN)
DRMTAP_GLES_CORE_SYMBOLS(DRMTAP_DECLARE_GL_PFN)
#undef DRMTAP_DECLARE_GL_PFN

static void *g_libegl_handle;
static void *g_libgles_handle;
static int g_gl_libs_state; /* 0 = not attempted, 1 = loaded, -1 = failed */

/* dlopen libEGL + libGLESv2 and resolve every core symbol this file calls.
 * Returns 0 when everything resolved, -1 otherwise (sticky either way).
 * Thread-safe. NOTE: the # / ## operators suppress macro expansion of the
 * symbol names, so this loader is immune to the name-routing defines below,
 * but it must stay textually ABOVE them so the pfn_ declarations resolve. */
static int load_gl_libraries(void) {
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lk);
    if (g_gl_libs_state == 0) {
        g_gl_libs_state = -1;
        g_libegl_handle = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!g_libegl_handle) {
            g_libegl_handle = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
        }
        g_libgles_handle = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!g_libgles_handle) {
            g_libgles_handle = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (g_libegl_handle && g_libgles_handle) {
            int ok = 1;
            /* *(void **)&pfn is the POSIX-documented way to store a dlsym
             * result into a function pointer without the pedantic
             * object-to-function pointer conversion warning. */
#define DRMTAP_LOAD_EGL_SYM(name) \
            *(void **)&pfn_##name = dlsym(g_libegl_handle, #name); \
            if (!pfn_##name) ok = 0;
#define DRMTAP_LOAD_GLES_SYM(name) \
            *(void **)&pfn_##name = dlsym(g_libgles_handle, #name); \
            if (!pfn_##name) ok = 0;
            DRMTAP_EGL_CORE_SYMBOLS(DRMTAP_LOAD_EGL_SYM)
            DRMTAP_GLES_CORE_SYMBOLS(DRMTAP_LOAD_GLES_SYM)
#undef DRMTAP_LOAD_EGL_SYM
#undef DRMTAP_LOAD_GLES_SYM
            if (ok) {
                g_gl_libs_state = 1;
            }
        }
    }
    int ret = g_gl_libs_state;
    pthread_mutex_unlock(&lk);
    return (ret == 1) ? 0 : -1;
}

/* Route every EGL/GLES call in the rest of this file through the loaded
 * pointers while keeping the standard API names at the call sites (the same
 * pattern GLAD-style loaders use). Anything below this point that names an
 * EGL/GLES core function is calling through its pfn_ pointer. */
#define eglBindAPI pfn_eglBindAPI
#define eglChooseConfig pfn_eglChooseConfig
#define eglCreateContext pfn_eglCreateContext
#define eglDestroyContext pfn_eglDestroyContext
#define eglGetDisplay pfn_eglGetDisplay
#define eglGetError pfn_eglGetError
#define eglGetProcAddress pfn_eglGetProcAddress
#define eglInitialize pfn_eglInitialize
#define eglMakeCurrent pfn_eglMakeCurrent
#define glActiveTexture pfn_glActiveTexture
#define glAttachShader pfn_glAttachShader
#define glBindAttribLocation pfn_glBindAttribLocation
#define glBindFramebuffer pfn_glBindFramebuffer
#define glBindTexture pfn_glBindTexture
#define glCompileShader pfn_glCompileShader
#define glCreateProgram pfn_glCreateProgram
#define glCreateShader pfn_glCreateShader
#define glDeleteFramebuffers pfn_glDeleteFramebuffers
#define glDeleteProgram pfn_glDeleteProgram
#define glDeleteShader pfn_glDeleteShader
#define glDeleteTextures pfn_glDeleteTextures
#define glDisableVertexAttribArray pfn_glDisableVertexAttribArray
#define glDrawArrays pfn_glDrawArrays
#define glEnableVertexAttribArray pfn_glEnableVertexAttribArray
#define glFinish pfn_glFinish
#define glFramebufferTexture2D pfn_glFramebufferTexture2D
#define glGenFramebuffers pfn_glGenFramebuffers
#define glGenTextures pfn_glGenTextures
#define glGetError pfn_glGetError
#define glGetProgramiv pfn_glGetProgramiv
#define glGetShaderiv pfn_glGetShaderiv
#define glGetUniformLocation pfn_glGetUniformLocation
#define glLinkProgram pfn_glLinkProgram
#define glReadPixels pfn_glReadPixels
#define glShaderSource pfn_glShaderSource
#define glTexImage2D pfn_glTexImage2D
#define glTexParameteri pfn_glTexParameteri
#define glUniform1f pfn_glUniform1f
#define glUniform1i pfn_glUniform1i
#define glUseProgram pfn_glUseProgram
#define glVertexAttribPointer pfn_glVertexAttribPointer
#define glViewport pfn_glViewport

/* ========================================================================= */
/* EGL context (lazily initialized per drmtap_ctx)                           */
/* ========================================================================= */

/* Global EGL display (one per process, shared across threads) */
static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static int g_egl_display_initialized = 0;

/* ── Import-once EGLImage cache (keyed by KMS fb_id) ── */
/* The compositor scans out of a small pool of framebuffers (double/triple
 * buffering), so the same fb_ids repeat every frame. Importing the DMA-BUF as
 * an EGLImage once per fb_id — instead of create+destroy per frame — removes
 * the biggest per-frame EGL cost. The cached EGLImage aliases the live
 * scanout memory, so sampling it always reads the current frame content.
 * Geometry is stored to detect a recycled fb_id (same id, new framebuffer)
 * and re-import. An EGLImage keeps its underlying buffer alive, so at most
 * DRMTAP_EGL_IMAGE_SLOTS scanout buffers are pinned per thread — the same
 * order of pinning the fast-grab mmap slots already do. */
#define DRMTAP_EGL_IMAGE_SLOTS 4
typedef struct {
    const drmtap_ctx *owner;    /* cache entries are per capture context */
    uint32_t fb_id;             /* 0 = slot unused */
    uint32_t width, height, stride, fourcc;
    uint64_t modifier;
    int      num_planes;
    uint32_t offsets[4];
    uint32_t pitches[4];
    ino_t    buf_inode;         /* dma-buf inode = the real buffer identity;
                                   0 = unknown (fd was -1 or fstat failed) */
    EGLImageKHR image;
    GLuint texture;             /* GL_TEXTURE_EXTERNAL_OES bound to image */
} egl_image_slot_t;

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
    egl_image_slot_t image_slots[DRMTAP_EGL_IMAGE_SLOTS];
    unsigned next_evict;  /* round-robin eviction cursor for image_slots */
    int cache_disabled;   /* DRMTAP_NO_IMAGE_CACHE=1 */
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
static int g_egl_tsd_key_ok = 0;   /* set once the key is created successfully */
static pthread_once_t g_egl_tsd_once = PTHREAD_ONCE_INIT;
static void egl_cleanup(egl_state_t *st);
static void egl_tsd_destructor(void *unused) {
    (void)unused;
    egl_cleanup(&state);
}
static void egl_tsd_key_init(void) {
    /* Only mark the key usable if creation succeeds; otherwise g_egl_tsd_key is
     * indeterminate and must never be passed to pthread_setspecific. */
    if (pthread_key_create(&g_egl_tsd_key, egl_tsd_destructor) == 0) {
        g_egl_tsd_key_ok = 1;
    }
}

/* EGL function pointers (loaded dynamically) */
static PFNEGLQUERYDEVICESEXTPROC pfn_eglQueryDevicesEXT;
static PFNEGLQUERYDEVICESTRINGEXTPROC pfn_eglQueryDeviceStringEXT;
static PFNEGLGETPLATFORMDISPLAYEXTPROC pfn_eglGetPlatformDisplayEXT;
static PFNEGLCREATEIMAGEKHRPROC pfn_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC pfn_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pfn_glEGLImageTargetTexture2DOES;

static int load_egl_procs(void) {
    /* Bring in libEGL/libGLESv2 first — every EGL entry point in this file
     * funnels through here (egl_init and drmtap_gpu_egl_available) before any
     * EGL/GL call is made, so this is the single lazy-load chokepoint. */
    if (load_gl_libraries() < 0) {
        return -1;
    }

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

/* From EGL_EXT_device_drm_render_node; missing from older eglext.h. */
#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif

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
        /* Match either the card path (drmtap_open) or the render node path
         * (drmtap_open_render) — a render-only context knows its GPU only by
         * its /dev/dri/renderD* node. The render-node query returns NULL when
         * the extension is absent, which just skips that comparison. */
        const char *drm_path = pfn_eglQueryDeviceStringEXT(
            devices[i], EGL_DRM_DEVICE_FILE_EXT);
        const char *render_path = pfn_eglQueryDeviceStringEXT(
            devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);
        if ((drm_path && strcmp(drm_path, card_path) == 0) ||
            (render_path && strcmp(render_path, card_path) == 0)) {
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
    /* Escape hatch: DRMTAP_NO_IMAGE_CACHE=1 forces a fresh EGLImage import
     * per frame (the pre-0.4.9 behaviour) for debugging driver oddities. */
    const char *nocache = getenv("DRMTAP_NO_IMAGE_CACHE");
    state->cache_disabled = (nocache && nocache[0] == '1');
    state->initialized = 1;
    /* Register the thread-exit backstop so a capture thread that dies without
     * drmtap_close() still frees this context (value must be non-NULL to arm the
     * destructor; the destructor frees the thread-local `state` directly). Skip
     * arming it if the key could not be created (indeterminate key => no UB). */
    pthread_once(&g_egl_tsd_once, egl_tsd_key_init);
    if (g_egl_tsd_key_ok) {
        pthread_setspecific(g_egl_tsd_key, state);
    }
    drmtap_debug_log(NULL, "EGL: init OK: display=%p ctx=%p program=%u fbo=%u (tid=%lu)",
            (void*)state->display, (void*)state->context, state->program, state->fbo,
            (unsigned long)pthread_self());

    drmtap_debug_log(ctx, "egl: GPU-universal backend initialized");
    return 0;
}

/* Release one cached EGLImage slot (external texture + image). The caller
 * must have the thread's EGL context current. Safe on an empty slot. */
static void egl_image_slot_release(egl_state_t *st, egl_image_slot_t *slot) {
    if (slot->texture) {
        glDeleteTextures(1, &slot->texture);
    }
    if (slot->image != EGL_NO_IMAGE_KHR && pfn_eglDestroyImageKHR) {
        pfn_eglDestroyImageKHR(st->display, slot->image);
    }
    memset(slot, 0, sizeof(*slot));
}

static void egl_cleanup(egl_state_t *st) {
    if (!st->initialized) {
        return;
    }
    eglMakeCurrent(st->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   st->context);
    /* Drop the fb_id EGLImage cache first: the GL textures need the context
     * current, and the cached EGLImages pin the imported scanout buffers. */
    for (int i = 0; i < DRMTAP_EGL_IMAGE_SLOTS; i++) {
        egl_image_slot_release(st, &st->image_slots[i]);
    }
    st->next_evict = 0;
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

/* Import a DMA-BUF as an EGLImage, retrying with progressively simpler
 * attribute sets (XRGB8888 keeping the modifier, then XRGB8888 alone) the way
 * some drivers need. CCS auxiliary planes come from ctx->fb2_*. Returns
 * EGL_NO_IMAGE_KHR when every attempt failed. */
static EGLImageKHR egl_import_dmabuf(drmtap_ctx *ctx, int dma_buf_fd,
                                     uint32_t width, uint32_t height,
                                     uint32_t stride, uint32_t fourcc,
                                     uint64_t modifier) {
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
            return EGL_NO_IMAGE_KHR;
        }
        drmtap_debug_log(NULL, "EGL: retry SUCCEEDED");
        drmtap_debug_log(ctx, "egl: retry succeeded");
    }
    return image;
}

/* Wrap an EGLImage in a GL_TEXTURE_EXTERNAL_OES texture (the GPU detiles
 * transparently when sampling it). Leaves the texture bound on unit 0. */
static int egl_make_external_texture(drmtap_ctx *ctx, EGLImageKHR image,
                                     GLuint *out_texture) {
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
        return -EIO;
    }
    *out_texture = ext_texture;
    return 0;
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
                            uint64_t modifier, uint32_t fb_id,
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
                           uint64_t modifier, uint32_t fb_id,
                           void **out_data, size_t *out_size) {
    static pthread_mutex_t g_convert_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&g_convert_lock);
    int _ret = egl_convert_impl(ctx, dma_buf_fd, width, height, stride, fourcc,
                                modifier, fb_id, out_data, out_size);
    pthread_mutex_unlock(&g_convert_lock);
    return _ret;
}

static int egl_convert_impl(drmtap_ctx *ctx,
                            int dma_buf_fd,
                            uint32_t width, uint32_t height,
                            uint32_t stride, uint32_t fourcc,
                            uint64_t modifier, uint32_t fb_id,
                            void **out_data, size_t *out_size) {
    if (!ctx || !out_data || !out_size) {
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

    /* ── Import-once: EGLImage cache keyed by fb_id ── */
    /* slot is only ever read through (the writes go through `dst`/`s`), so it
     * is const; free_slot/dst stay mutable because they populate a slot. */
    const egl_image_slot_t *slot = NULL;
    egl_image_slot_t *free_slot = NULL;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint ext_texture = 0;
    int use_cache = (fb_id != 0 && !state.cache_disabled);

    /* Real buffer identity: a DMA-BUF has a unique inode that is stable across
     * re-exports of the same underlying scanout BO and changes when the BO
     * changes. KMS recycles a freed fb_id onto a brand-new BO that can have
     * BYTE-IDENTICAL geometry, so geometry alone cannot tell "same fb, live
     * buffer" from "same id, new buffer" — the inode can. Captured when a real
     * fd is supplied; 0 (fd == -1, the known-cached contract, or fstat failed)
     * means "trust fb_id + geometry". */
    ino_t cur_inode = 0;
    if (dma_buf_fd >= 0) {
        struct stat st;
        if (fstat(dma_buf_fd, &st) == 0) {
            cur_inode = st.st_ino;
        }
    }

    if (use_cache) {
        for (int i = 0; i < DRMTAP_EGL_IMAGE_SLOTS; i++) {
            egl_image_slot_t *s = &state.image_slots[i];
            if (s->fb_id == 0) {
                if (!free_slot) {
                    free_slot = s;
                }
                continue;
            }
            if (s->fb_id != fb_id || s->owner != ctx) {
                continue;
            }
            int geom_match =
                (s->width == width && s->height == height &&
                 s->stride == stride && s->fourcc == fourcc &&
                 s->modifier == modifier &&
                 s->num_planes == ctx->fb2_num_planes &&
                 memcmp(s->offsets, ctx->fb2_offsets, sizeof(s->offsets)) == 0 &&
                 memcmp(s->pitches, ctx->fb2_pitches, sizeof(s->pitches)) == 0);
            /* Identity holds unless BOTH inodes are known and differ — i.e. a
             * recycled fb_id on a new buffer. If either is unknown, geometry
             * is the best signal we have and we trust it. */
            int ident_match =
                (cur_inode == 0 || s->buf_inode == 0 ||
                 s->buf_inode == cur_inode);
            if (geom_match && ident_match) {
                slot = s;
                break;
            }
            /* Different geometry, or same geometry but the buffer identity
             * changed (fb_id recycled onto a new BO) — the cached import is
             * stale. Drop it and reuse the slot for the fresh import. */
            egl_image_slot_release(&state, s);
            if (!free_slot) {
                free_slot = s;
            }
        }
    }

    if (slot) {
        /* Cache hit: the EGLImage aliases the live scanout buffer, so binding
         * the cached external texture samples the CURRENT frame content — no
         * re-import, no texture churn. */
        image = slot->image;
        ext_texture = slot->texture;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, ext_texture);
    } else {
        if (dma_buf_fd < 0) {
            drmtap_set_error(ctx, "egl: fb_id %u is not cached and no "
                             "dma-buf fd was supplied", fb_id);
            return -EINVAL;
        }
        image = egl_import_dmabuf(ctx, dma_buf_fd, width, height, stride,
                                  fourcc, modifier);
        if (image == EGL_NO_IMAGE_KHR) {
            return -EIO;
        }
        ret = egl_make_external_texture(ctx, image, &ext_texture);
        if (ret != 0) {
            pfn_eglDestroyImageKHR(state.display, image);
            return ret;
        }
        if (use_cache) {
            /* Adopt the fresh import into a slot: reuse a free one, else
             * round-robin evict (compositors cycle 2-3 buffers, so with 4
             * slots eviction only happens on pathological flip patterns). */
            egl_image_slot_t *dst = free_slot;
            if (!dst) {
                dst = &state.image_slots[state.next_evict %
                                         DRMTAP_EGL_IMAGE_SLOTS];
                state.next_evict++;
                egl_image_slot_release(&state, dst);
            }
            dst->owner = ctx;
            dst->fb_id = fb_id;
            dst->width = width;
            dst->height = height;
            dst->stride = stride;
            dst->fourcc = fourcc;
            dst->modifier = modifier;
            dst->num_planes = ctx->fb2_num_planes;
            memcpy(dst->offsets, ctx->fb2_offsets, sizeof(dst->offsets));
            memcpy(dst->pitches, ctx->fb2_pitches, sizeof(dst->pitches));
            dst->buf_inode = cur_inode;
            dst->image = image;
            dst->texture = ext_texture;
            slot = dst;
        }
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
     * from the connector HDR_OUTPUT_METADATA (carried on ctx). ctx is
     * non-NULL here — the function returns -EINVAL up top otherwise. */
    int hdr_on = (ctx->cur_hdr_eotf == DRMTAP_EOTF_PQ) ? 1 : 0;
    glUniform1i(glGetUniformLocation(state.program, "u_hdr"), hdr_on);
    {
        uint32_t mn = (ctx->cur_hdr_max_nits > 0)
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

    /* Read linear pixels straight into the ctx-owned grow-once buffer
     * (drmtap_ensure_buf caps it at DRMTAP_MAX_FB_BYTES), so steady-state
     * conversion performs zero per-frame allocations. The buffer belongs to
     * the context and is freed in drmtap_close(); callers must not free it. */
    size_t rgba_size = (size_t)width * height * 4;
    ret = drmtap_ensure_buf(&ctx->deswizzle_buf, &ctx->deswizzle_buf_size,
                            rgba_size);
    if (ret != 0) {
        goto cleanup;
    }
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                 ctx->deswizzle_buf);
    drmtap_debug_log(NULL, "EGL: glReadPixels done, gl_err=0x%x", glGetError());

    /* NOTE: CPU horizontal flip removed. The EGL DMA-BUF import with
     * standard texcoords (no shader manipulation) produces correct
     * orientation. The previous CPU flip was compensating for a shader-
     * based X flip (1.0 - v_texcoord.x) that has since been removed. */

    *out_data = ctx->deswizzle_buf;
    *out_size = rgba_size;
    ret = 0;

    drmtap_debug_log(ctx, "egl: converted %ux%u tiled→linear (%zu bytes)",
                     width, height, rgba_size);

cleanup:
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    if (!slot) {
        /* Uncached import (fb_id 0, or cache disabled): per-call lifetime,
         * exactly the pre-cache behaviour. A cached import stays alive in its
         * slot until eviction or egl_cleanup(). */
        glDeleteTextures(1, &ext_texture);
        pfn_eglDestroyImageKHR(state.display, image);
    }

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
                            uint64_t modifier, uint32_t fb_id,
                            void **out_data, size_t *out_size) {
    (void)ctx; (void)dma_buf_fd; (void)width; (void)height;
    (void)stride; (void)fourcc; (void)modifier; (void)fb_id;
    (void)out_data; (void)out_size;
    drmtap_set_error(ctx, "EGL backend not available (built without EGL)");
    return -ENOTSUP;
}

void drmtap_gpu_egl_thread_cleanup(void) {
    /* No EGL backend compiled in — nothing to release. */
}

#endif /* HAVE_EGL */
