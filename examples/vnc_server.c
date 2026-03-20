/*
 * libdrmtap — DRM/KMS screen capture library for Linux
 * https://github.com/fxd0h/libdrmtap
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * @file vnc_server.c
 * @brief VNC server using libdrmtap — full remote desktop proof of concept
 *
 * Demonstrates libdrmtap in a real remote desktop scenario:
 *   - Captures the screen via DRM/KMS (login screen, Wayland, headless)
 *   - Injects mouse and keyboard via uinput
 *   - Serves it over VNC — connect from any VNC client
 *   - No PipeWire, no user prompts, no compositor cooperation
 *
 * Build:
 *   gcc -o vnc_server vnc_server.c \
 *       $(pkg-config --cflags --libs libdrmtap) \
 *       $(pkg-config --cflags --libs libvncserver)
 *
 * Run:
 *   sudo ./vnc_server                # needs uinput access
 *
 * Connect:
 *   From macOS: open vnc://VM_IP:5900
 *   Password: drmtap
 */

#define _DEFAULT_SOURCE  /* expose usleep() with -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/uinput.h>
#include <linux/input-event-codes.h>
#include <drmtap.h>
#include <rfb/rfb.h>
#include <rfb/keysym.h>

static volatile int running = 1;
static int uinput_mouse_fd = -1;
static int uinput_kbd_fd = -1;
static int screen_width = 0;
static int screen_height = 0;
static int debug_input = 0;

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

/* ============================================================
 * uinput — virtual mouse and keyboard for input injection
 * ============================================================ */

static void emit(int fd, int type, int code, int val) {
    struct input_event ev = {0};
    ev.type = type;
    ev.code = code;
    ev.value = val;
    if (write(fd, &ev, sizeof(ev)) < 0) {
        /* silently ignore write errors */
    }
}

static void emit_syn(int fd) {
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

static int setup_uinput_mouse(int w, int h) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open /dev/uinput (mouse)");
        return -1;
    }

    /* Enable event types */
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);

    /* Mouse buttons */
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);

    /* Mark as direct input device (like touchscreen) so compositor picks it up */
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    /* Absolute axes for pointer position */
    struct uinput_abs_setup abs_x = {0};
    abs_x.code = ABS_X;
    abs_x.absinfo.minimum = 0;
    abs_x.absinfo.maximum = w - 1;
    ioctl(fd, UI_ABS_SETUP, &abs_x);

    struct uinput_abs_setup abs_y = {0};
    abs_y.code = ABS_Y;
    abs_y.absinfo.minimum = 0;
    abs_y.absinfo.maximum = h - 1;
    ioctl(fd, UI_ABS_SETUP, &abs_y);

    /* Scroll wheel */
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);

    struct uinput_setup setup = {0};
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "libdrmtap-vnc-mouse");
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x1234;
    setup.id.product = 0x0001;
    ioctl(fd, UI_DEV_SETUP, &setup);
    ioctl(fd, UI_DEV_CREATE);

    printf("  uinput mouse: OK (%dx%d)\n", w, h);
    return fd;
}

static int setup_uinput_keyboard(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open /dev/uinput (keyboard)");
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);

    /* Enable all standard keys (1-255) */
    for (int i = 1; i < 256; i++) {
        ioctl(fd, UI_SET_KEYBIT, i);
    }

    struct uinput_setup setup = {0};
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "libdrmtap-vnc-kbd");
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x1234;
    setup.id.product = 0x0002;
    ioctl(fd, UI_DEV_SETUP, &setup);
    ioctl(fd, UI_DEV_CREATE);

    printf("  uinput keyboard: OK\n");
    return fd;
}

static void cleanup_uinput(void) {
    if (uinput_mouse_fd >= 0) {
        ioctl(uinput_mouse_fd, UI_DEV_DESTROY);
        close(uinput_mouse_fd);
    }
    if (uinput_kbd_fd >= 0) {
        ioctl(uinput_kbd_fd, UI_DEV_DESTROY);
        close(uinput_kbd_fd);
    }
}

/* ============================================================
 * VNC keysym → Linux keycode mapping
 * ============================================================ */

static int keysym_to_keycode(rfbKeySym keysym) {
    /* Linux keycodes follow QWERTY physical layout, NOT alphabetical order.
     * We need an explicit lookup table for a-z. */
    static const int alpha_keycodes[26] = {
        /* a=*/ KEY_A,  /* b=*/ KEY_B,  /* c=*/ KEY_C,  /* d=*/ KEY_D,
        /* e=*/ KEY_E,  /* f=*/ KEY_F,  /* g=*/ KEY_G,  /* h=*/ KEY_H,
        /* i=*/ KEY_I,  /* j=*/ KEY_J,  /* k=*/ KEY_K,  /* l=*/ KEY_L,
        /* m=*/ KEY_M,  /* n=*/ KEY_N,  /* o=*/ KEY_O,  /* p=*/ KEY_P,
        /* q=*/ KEY_Q,  /* r=*/ KEY_R,  /* s=*/ KEY_S,  /* t=*/ KEY_T,
        /* u=*/ KEY_U,  /* v=*/ KEY_V,  /* w=*/ KEY_W,  /* x=*/ KEY_X,
        /* y=*/ KEY_Y,  /* z=*/ KEY_Z,
    };

    if (keysym >= XK_a && keysym <= XK_z)
        return alpha_keycodes[keysym - XK_a];
    if (keysym >= XK_A && keysym <= XK_Z)
        return alpha_keycodes[keysym - XK_A];
    if (keysym >= XK_0 && keysym <= XK_9)
        return KEY_0 + (keysym - XK_0);
    if (keysym >= XK_F1 && keysym <= XK_F12)
        return KEY_F1 + (keysym - XK_F1);

    switch (keysym) {
    case XK_Return:     return KEY_ENTER;
    case XK_Escape:     return KEY_ESC;
    case XK_BackSpace:  return KEY_BACKSPACE;
    case XK_Tab:        return KEY_TAB;
    case XK_space:      return KEY_SPACE;
    case XK_Shift_L:    return KEY_LEFTSHIFT;
    case XK_Shift_R:    return KEY_RIGHTSHIFT;
    case XK_Control_L:  return KEY_LEFTCTRL;
    case XK_Control_R:  return KEY_RIGHTCTRL;
    case XK_Alt_L:      return KEY_LEFTALT;
    case XK_Alt_R:      return KEY_RIGHTALT;
    case XK_Super_L:    return KEY_LEFTMETA;
    case XK_Super_R:    return KEY_RIGHTMETA;
    case XK_Caps_Lock:  return KEY_CAPSLOCK;
    case XK_Delete:     return KEY_DELETE;
    case XK_Home:       return KEY_HOME;
    case XK_End:        return KEY_END;
    case XK_Page_Up:    return KEY_PAGEUP;
    case XK_Page_Down:  return KEY_PAGEDOWN;
    case XK_Up:         return KEY_UP;
    case XK_Down:       return KEY_DOWN;
    case XK_Left:       return KEY_LEFT;
    case XK_Right:      return KEY_RIGHT;
    case XK_Insert:     return KEY_INSERT;
    case XK_minus:      return KEY_MINUS;
    case XK_equal:      return KEY_EQUAL;
    case XK_bracketleft:  return KEY_LEFTBRACE;
    case XK_bracketright: return KEY_RIGHTBRACE;
    case XK_semicolon:  return KEY_SEMICOLON;
    case XK_apostrophe: return KEY_APOSTROPHE;
    case XK_grave:      return KEY_GRAVE;
    case XK_backslash:  return KEY_BACKSLASH;
    case XK_comma:      return KEY_COMMA;
    case XK_period:     return KEY_DOT;
    case XK_slash:      return KEY_SLASH;
    default:            return -1;
    }
}

/* ============================================================
 * VNC callbacks for mouse and keyboard events
 * ============================================================ */

static int last_button_mask = 0;

static void vnc_ptr_event(int button_mask, int x, int y,
                          rfbClientPtr cl) {
    (void)cl;
    if (uinput_mouse_fd < 0) return;

    if (debug_input) {
        fprintf(stderr, "PTR: x=%d y=%d mask=0x%x\n", x, y, button_mask);
    }

    /* Move pointer (absolute) */
    emit(uinput_mouse_fd, EV_ABS, ABS_X, x);
    emit(uinput_mouse_fd, EV_ABS, ABS_Y, y);

    /* Button changes */
    if ((button_mask & 1) != (last_button_mask & 1)) {
        int down = (button_mask & 1) ? 1 : 0;
        emit(uinput_mouse_fd, EV_KEY, BTN_LEFT, down);
        /* Emit BTN_TOUCH too — some Wayland compositors require this for direct input devices */
        emit(uinput_mouse_fd, EV_KEY, BTN_TOUCH, down);
    }
    if ((button_mask & 2) != (last_button_mask & 2))
        emit(uinput_mouse_fd, EV_KEY, BTN_MIDDLE, (button_mask & 2) ? 1 : 0);
    if ((button_mask & 4) != (last_button_mask & 4))
        emit(uinput_mouse_fd, EV_KEY, BTN_RIGHT, (button_mask & 4) ? 1 : 0);

    /* Scroll wheel: VNC sends button 4/5 for scroll */
    if (button_mask & 8)
        emit(uinput_mouse_fd, EV_REL, REL_WHEEL, 1);   /* scroll up */
    if (button_mask & 16)
        emit(uinput_mouse_fd, EV_REL, REL_WHEEL, -1);  /* scroll down */

    emit_syn(uinput_mouse_fd);
    last_button_mask = button_mask;
}

static void vnc_kbd_event(rfbBool down, rfbKeySym keysym,
                          rfbClientPtr cl) {
    (void)cl;
    if (uinput_kbd_fd < 0) return;

    if (debug_input) {
        fprintf(stderr, "KBD: sym=0x%x down=%d\n", (unsigned int)keysym, down);
    }

    int keycode = keysym_to_keycode(keysym);
    if (keycode < 0) return;

    emit(uinput_kbd_fd, EV_KEY, keycode, down ? 1 : 0);
    emit_syn(uinput_kbd_fd);
}

/* ============================================================
 * Main
 * ============================================================ */

int main(int argc, char **argv) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* Open DRM capture context */
    debug_input = (getenv("DRMTAP_DEBUG") != NULL);
    drmtap_config cfg = {0};
    cfg.debug = debug_input;

    drmtap_ctx *ctx = drmtap_open(&cfg);
    if (!ctx) {
        fprintf(stderr, "drmtap_open failed: %s\n",
                drmtap_error(NULL) ? drmtap_error(NULL) : "unknown");
        return 1;
    }

    printf("GPU: %s\n", drmtap_gpu_driver(ctx));

    /* List displays */
    drmtap_display displays[8];
    int n = drmtap_list_displays(ctx, displays, 8);
    if (n <= 0) {
        fprintf(stderr, "No displays found\n");
        drmtap_close(ctx);
        return 1;
    }

    for (int i = 0; i < n; i++) {
        printf("  [%d] %s: %ux%u@%uHz (active=%d)\n",
               i, displays[i].name, displays[i].width, displays[i].height,
               displays[i].refresh_hz, displays[i].active);
    }

    /* Grab one frame to get dimensions */
    drmtap_frame_info frame = {0};
    int ret = drmtap_grab_mapped(ctx, &frame);
    if (ret < 0) {
        fprintf(stderr, "Initial grab failed: %s\n", drmtap_error(ctx));
        drmtap_close(ctx);
        return 1;
    }

    screen_width = frame.width;
    screen_height = frame.height;
    int stride = frame.stride;
    int bpp = 4;

    printf("Captured: %dx%d stride=%d\n", screen_width, screen_height, stride);

    /* Set up uinput for mouse and keyboard */
    printf("Input devices:\n");
    uinput_mouse_fd = setup_uinput_mouse(screen_width, screen_height);
    uinput_kbd_fd = setup_uinput_keyboard();
    if (uinput_mouse_fd < 0 || uinput_kbd_fd < 0) {
        fprintf(stderr, "Warning: uinput failed — input disabled. Run with sudo.\n");
    }
    /* Small delay for uinput device to register */
    usleep(200000);

    /* Initialize VNC server */
    rfbScreenInfoPtr server = rfbGetScreen(&argc, argv,
                                            screen_width, screen_height,
                                            8, 3, bpp);
    if (!server) {
        fprintf(stderr, "rfbGetScreen failed\n");
        drmtap_frame_release(ctx, &frame);
        drmtap_close(ctx);
        cleanup_uinput();
        return 1;
    }

    server->desktopName = "libdrmtap VNC";
    server->alwaysShared = TRUE;
    server->port = 5900;

    /* Input callbacks */
    server->ptrAddEvent = vnc_ptr_event;
    server->kbdAddEvent = vnc_kbd_event;

    /* VNC password auth */
    {
        const char *passwd_file = "/tmp/.drmtap_vnc_passwd";
        char *passwords[2] = { "drmtap", NULL };
        rfbEncryptAndStorePasswd(passwords[0], (char *)passwd_file);
        server->authPasswdData = (void *)passwd_file;
    }

    /* Allocate framebuffer */
    server->frameBuffer = (char *)malloc(screen_width * screen_height * bpp);
    if (!server->frameBuffer) {
        fprintf(stderr, "malloc failed\n");
        drmtap_frame_release(ctx, &frame);
        drmtap_close(ctx);
        cleanup_uinput();
        return 1;
    }

    /* Pixel format: DRM XRGB8888 (little-endian = BGRX in memory) */
    server->serverFormat.redShift = 16;
    server->serverFormat.greenShift = 8;
    server->serverFormat.blueShift = 0;
    server->serverFormat.redMax = 255;
    server->serverFormat.greenMax = 255;
    server->serverFormat.blueMax = 255;
    server->serverFormat.bitsPerPixel = 32;
    server->serverFormat.depth = 24;

    /* Copy first frame */
    if (frame.data) {
        for (int y = 0; y < screen_height; y++) {
            memcpy(server->frameBuffer + y * screen_width * bpp,
                   (char *)frame.data + y * stride,
                   screen_width * bpp);
        }
    }
    drmtap_frame_release(ctx, &frame);

    /* Start VNC server */
    rfbInitServer(server);
    printf("\n=== libdrmtap VNC server running on port %d ===\n", server->port);
    printf("Connect: vnc://THIS_IP:5900  (password: drmtap)\n");
    printf("Input:   %s\n",
           (uinput_mouse_fd >= 0) ? "mouse + keyboard enabled" : "DISABLED");
    printf("Press Ctrl+C to stop.\n\n");

    /* Capture loop */
    int fps = 0;
    int skipped = 0;
    uint32_t last_fb_id = 0;
    time_t last_time = 0;

    while (running && rfbIsActive(server)) {
        long timeout_us = 16666;
        rfbProcessEvents(server, timeout_us);

        memset(&frame, 0, sizeof(frame));
        ret = drmtap_grab_mapped(ctx, &frame);
        if (ret < 0) {
            usleep(100000);
            continue;
        }

        /* Skip EGL detiling + memcpy if framebuffer hasn't changed */
        if (frame.fb_id == last_fb_id) {
            drmtap_frame_release(ctx, &frame);
            skipped++;
            usleep(16666);  /* ~60 Hz poll rate */
            continue;
        }
        last_fb_id = frame.fb_id;

        if (frame.data) {
            for (int y = 0; y < screen_height; y++) {
                memcpy(server->frameBuffer + y * screen_width * bpp,
                       (char *)frame.data + y * stride,
                       screen_width * bpp);
            }
            rfbMarkRectAsModified(server, 0, 0, screen_width, screen_height);
        }
        drmtap_frame_release(ctx, &frame);

        fps++;
        time_t now = time(NULL);
        if (now != last_time) {
            printf("\rFPS: %d (skipped: %d)    ", fps, skipped);
            fflush(stdout);
            fps = 0;
            skipped = 0;
            last_time = now;
        }
    }

    printf("\nShutting down...\n");
    free(server->frameBuffer);
    rfbScreenCleanup(server);
    cleanup_uinput();
    drmtap_close(ctx);
    printf("Done.\n");
    return 0;
}
