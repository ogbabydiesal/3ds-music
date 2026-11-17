// osc3ds — turn a Nintendo 3DS into a wireless OSC controller.
//
// Top screen:    status / help (text console)
// Bottom screen: touch control surface with 4 tabs: XY | FADERS | PADS | MOTION
//
// All messages go out as OSC over UDP to the host/port in
// sdmc:/3ds/osc3ds/config.ini (editable on-device with SELECT).

#include <3ds.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "osc.h"

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000
#define CONFIG_DIR      "sdmc:/3ds/osc3ds"
#define CONFIG_PATH     CONFIG_DIR "/config.ini"

// ---------------------------------------------------------------- config ---

typedef struct {
    char host[64];
    int  port;
    char prefix[32];
} Config;

static Config cfg = { "192.168.1.100", 9000, "/3ds" };

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static void config_load(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == ';') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k); trim(v);
        if (!strcmp(k, "host"))        snprintf(cfg.host, sizeof cfg.host, "%s", v);
        else if (!strcmp(k, "port"))   cfg.port = atoi(v);
        else if (!strcmp(k, "prefix")) snprintf(cfg.prefix, sizeof cfg.prefix, "%s", v);
    }
    fclose(f);
}

static void config_save(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "# osc3ds config\nhost=%s\nport=%d\nprefix=%s\n", cfg.host, cfg.port, cfg.prefix);
    fclose(f);
}

// ------------------------------------------------------------------- net ---

static int sock = -1;
static struct sockaddr_in dest;
static unsigned long sent_count = 0, err_count = 0;
static char last_msg[64] = "";

static void net_set_dest(void) {
    memset(&dest, 0, sizeof dest);
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)cfg.port);
    dest.sin_addr.s_addr = inet_addr(cfg.host);
}

static void osc_send_raw(const char *addr, uint8_t *buf, size_t len) {
    if (len == 0) return;
    if (sendto(sock, buf, len, 0, (struct sockaddr *)&dest, sizeof dest) < 0) err_count++;
    else sent_count++;
    snprintf(last_msg, sizeof last_msg, "%s", addr);
}

// Send `n` floats to <prefix><path>.
static void send_f(const char *path, int n, float a, float b, float c) {
    char addr[96];
    uint8_t buf[256];
    snprintf(addr, sizeof addr, "%s%s", cfg.prefix, path);
    size_t len = 0;
    switch (n) {
    case 1: len = osc_encode(buf, sizeof buf, addr, "f", a); break;
    case 2: len = osc_encode(buf, sizeof buf, addr, "ff", a, b); break;
    case 3: len = osc_encode(buf, sizeof buf, addr, "fff", a, b, c); break;
    }
    osc_send_raw(addr, buf, len);
}

// ------------------------------------------------------------ drawing ---
// Bottom framebuffer is 240x320 BGR8, rotated: screen (x,y) -> column-major.

static u8 *fb;

static inline void px(int x, int y, u32 rgb) {
    if ((unsigned)x >= 320 || (unsigned)y >= 240) return;
    u8 *p = fb + ((x * 240) + (239 - y)) * 3;
    p[0] = rgb & 0xFF; p[1] = (rgb >> 8) & 0xFF; p[2] = (rgb >> 16) & 0xFF;
}

static void rect(int x0, int y0, int w, int h, u32 rgb) {
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > 320) w = 320 - x0;
    if (y0 + h > 240) h = 240 - y0;
    if (w <= 0 || h <= 0) return;
    u8 b = rgb & 0xFF, g = (rgb >> 8) & 0xFF, r = (rgb >> 16) & 0xFF;
    for (int x = x0; x < x0 + w; x++) {
        u8 *col = fb + (x * 240 + (239 - (y0 + h - 1))) * 3;
        for (int i = 0; i < h; i++) { col[0] = b; col[1] = g; col[2] = r; col += 3; }
    }
}

static void frame(int x0, int y0, int w, int h, u32 rgb) {
    rect(x0, y0, w, 1, rgb); rect(x0, y0 + h - 1, w, 1, rgb);
    rect(x0, y0, 1, h, rgb); rect(x0 + w - 1, y0, 1, h, rgb);
}

#define COL_BG      0x101418
#define COL_PANEL   0x1E252C
#define COL_LINE    0x3A4550
#define COL_ACCENT  0x34C3A0
#define COL_HOT     0xF2B134
#define COL_OFF     0x5A2A2A

// ------------------------------------------------------------- surface ---

enum { MODE_XY, MODE_FADERS, MODE_PADS };
static const char *mode_names[] = { "XY PAD", "FADERS", "PADS" };

#define TAB_H     30
#define AREA_Y    TAB_H
#define AREA_H    (240 - TAB_H)
#define NFADERS   4
#define PAD_COLS  4
#define PAD_ROWS  3
#define NPADS     (PAD_COLS * PAD_ROWS)

static int   mode = MODE_XY;
static bool  motion_on = false;
static float xy_x = 0.5f, xy_y = 0.5f;
static bool  xy_down = false;
static float faders[NFADERS] = { 0 };
static int   active_fader = -1;
static int   active_pad = -1;

static void draw_surface(void) {
    rect(0, 0, 320, 240, COL_BG);

    // Tab bar: 3 mode tabs + motion toggle
    for (int i = 0; i < 4; i++) {
        u32 c = COL_PANEL;
        if (i < 3 && i == mode) c = COL_ACCENT;
        if (i == 3) c = motion_on ? COL_HOT : COL_OFF;
        rect(i * 80 + 2, 2, 76, TAB_H - 4, c);
    }

    switch (mode) {
    case MODE_XY: {
        frame(4, AREA_Y + 4, 312, AREA_H - 8, COL_LINE);
        int cx = (int)(xy_x * 319), cy = AREA_Y + (int)((1.0f - xy_y) * (AREA_H - 1));
        u32 c = xy_down ? COL_HOT : COL_ACCENT;
        rect(cx, AREA_Y + 4, 1, AREA_H - 8, COL_LINE);
        rect(4, cy, 312, 1, COL_LINE);
        rect(cx - 6, cy - 6, 13, 13, c);
        break;
    }
    case MODE_FADERS:
        for (int i = 0; i < NFADERS; i++) {
            int x = i * 80 + 14, w = 52;
            rect(x, AREA_Y + 6, w, AREA_H - 12, COL_PANEL);
            int h = (int)(faders[i] * (AREA_H - 12));
            rect(x, AREA_Y + 6 + (AREA_H - 12 - h), w, h, i == active_fader ? COL_HOT : COL_ACCENT);
        }
        break;
    case MODE_PADS: {
        int pw = 320 / PAD_COLS, ph = AREA_H / PAD_ROWS;
        for (int i = 0; i < NPADS; i++) {
            int c = i % PAD_COLS, r = i / PAD_COLS;
            rect(c * pw + 4, AREA_Y + r * ph + 4, pw - 8, ph - 8, i == active_pad ? COL_HOT : COL_PANEL);
        }
        break;
    }
    }
}

static void pad_release(void) {
    if (active_pad >= 0) {
        char p[24]; snprintf(p, sizeof p, "/pad/%d", active_pad + 1);
        send_f(p, 1, 0, 0, 0);
        active_pad = -1;
    }
}

static void handle_touch(u32 kDown, u32 kHeld, u32 kUp) {
    touchPosition t;
    hidTouchRead(&t);

    // Tab bar: only acts on the initial press
    if ((kDown & KEY_TOUCH) && t.py < TAB_H) {
        int tab = t.px / 80;
        if (tab == 3) {
            motion_on = !motion_on;
            send_f("/motion/enabled", 1, motion_on ? 1 : 0, 0, 0);
        } else if (tab != mode) {
            pad_release();
            mode = tab;
        }
        return;
    }

    if (kUp & KEY_TOUCH) {
        if (xy_down) { xy_down = false; send_f("/xy/touch", 1, 0, 0, 0); }
        active_fader = -1;
        pad_release();
        return;
    }
    if (!(kHeld & KEY_TOUCH) || t.py < TAB_H) return;

    float nx = t.px / 319.0f;
    float ny = 1.0f - (t.py - AREA_Y) / (float)(AREA_H - 1);
    if (ny < 0) ny = 0;
    if (ny > 1) ny = 1;

    switch (mode) {
    case MODE_XY:
        if (!xy_down) { xy_down = true; send_f("/xy/touch", 1, 1, 0, 0); }
        if (fabsf(nx - xy_x) > 0.002f || fabsf(ny - xy_y) > 0.002f) {
            xy_x = nx; xy_y = ny;
            send_f("/xy", 2, xy_x, xy_y, 0);
        }
        break;
    case MODE_FADERS: {
        int i = t.px / 80;
        if (kDown & KEY_TOUCH) active_fader = i;
        if (active_fader < 0) break;
        float v = (t.py - (AREA_Y + 6)) / (float)(AREA_H - 12);
        v = 1.0f - (v < 0 ? 0 : v > 1 ? 1 : v);
        if (fabsf(v - faders[active_fader]) > 0.002f) {
            faders[active_fader] = v;
            char p[24]; snprintf(p, sizeof p, "/fader/%d", active_fader + 1);
            send_f(p, 1, v, 0, 0);
        }
        break;
    }
    case MODE_PADS: {
        int c = t.px / (320 / PAD_COLS), r = (t.py - AREA_Y) / (AREA_H / PAD_ROWS);
        if (c >= PAD_COLS) c = PAD_COLS - 1;
        if (r >= PAD_ROWS) r = PAD_ROWS - 1;
        int i = r * PAD_COLS + c;
        if (i != active_pad) {           // slide between pads re-triggers
            pad_release();
            active_pad = i;
            char p[24]; snprintf(p, sizeof p, "/pad/%d", i + 1);
            send_f(p, 1, 1, 0, 0);
        }
        break;
    }
    }
}

// ------------------------------------------------------------- buttons ---

static const struct { u32 key; const char *name; } buttons[] = {
    { KEY_A, "a" }, { KEY_B, "b" }, { KEY_X, "x" }, { KEY_Y, "y" },
    { KEY_L, "l" }, { KEY_R, "r" }, { KEY_ZL, "zl" }, { KEY_ZR, "zr" },
    { KEY_DUP, "up" }, { KEY_DDOWN, "down" }, { KEY_DLEFT, "left" }, { KEY_DRIGHT, "right" },
};

static void handle_buttons(u32 kDown, u32 kUp) {
    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++) {
        char p[32];
        if (kDown & buttons[i].key) {
            snprintf(p, sizeof p, "/button/%s", buttons[i].name);
            send_f(p, 1, 1, 0, 0);
        }
        if (kUp & buttons[i].key) {
            snprintf(p, sizeof p, "/button/%s", buttons[i].name);
            send_f(p, 1, 0, 0, 0);
        }
    }
}

static float stick_norm(int v) {
    float f = v / 156.0f;            // circle pad range is roughly +/-156
    if (fabsf(f) < 0.06f) f = 0;     // deadzone
    return f > 1 ? 1 : f < -1 ? -1 : f;
}

static void handle_sticks(void) {
    static float last_cx = 0, last_cy = 0, last_sx = 0, last_sy = 0;
    circlePosition cp, cs;
    hidCircleRead(&cp);
    hidCstickRead(&cs);   // New 3DS only; reads 0 on original models

    float cx = stick_norm(cp.dx), cy = stick_norm(cp.dy);
    if (fabsf(cx - last_cx) > 0.01f || fabsf(cy - last_cy) > 0.01f) {
        last_cx = cx; last_cy = cy;
        send_f("/circle", 2, cx, cy, 0);
    }
    float sx = stick_norm(cs.dx), sy = stick_norm(cs.dy);
    if (fabsf(sx - last_sx) > 0.01f || fabsf(sy - last_sy) > 0.01f) {
        last_sx = sx; last_sy = sy;
        send_f("/cstick", 2, sx, sy, 0);
    }
}

static void handle_motion(u32 frame_no) {
    if (!motion_on || (frame_no & 1)) return;   // ~30 Hz
    accelVector a;
    angularRate g;
    hidAccelRead(&a);
    hidGyroRead(&g);
    send_f("/accel", 3, a.x / 512.0f, a.y / 512.0f, a.z / 512.0f);   // ~1.0 = 1g
    send_f("/gyro", 3, g.x / 100.0f, g.y / 100.0f, g.z / 100.0f);
}

// ------------------------------------------------------------- settings ---

static bool keyboard_input(const char *hint, const char *initial, char *out, size_t outlen) {
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, (int)outlen - 1);
    swkbdSetNumpadKeys(&kb, L'.', 0);
    swkbdSetHintText(&kb, hint);
    swkbdSetInitialText(&kb, initial);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    char tmp[64];
    SwkbdButton btn = swkbdInputText(&kb, tmp, sizeof tmp);
    if (btn != SWKBD_BUTTON_CONFIRM) return false;
    snprintf(out, outlen, "%s", tmp);
    return true;
}

static void edit_settings(void) {
    char buf[64];
    if (keyboard_input("Target IP address", cfg.host, buf, sizeof buf))
        snprintf(cfg.host, sizeof cfg.host, "%s", buf);
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", cfg.port);
    if (keyboard_input("Target UDP port", portstr, buf, 6)) {
        int p = atoi(buf);
        if (p > 0 && p < 65536) cfg.port = p;
    }
    config_save();
    net_set_dest();
}

// ----------------------------------------------------------------- main ---

static void draw_status(void) {
    u32 ip = (u32)gethostid();
    printf("\x1b[1;1H\x1b[36m osc3ds\x1b[0m - 3DS OSC controller          ");
    printf("\x1b[3;1H 3DS IP:  %lu.%lu.%lu.%lu            ",
           ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
    printf("\x1b[4;1H Target:  %s:%d               ", cfg.host, cfg.port);
    printf("\x1b[5;1H Prefix:  %s                    ", cfg.prefix);
    printf("\x1b[7;1H Mode:    %-8s  Motion: %s   ", mode_names[mode], motion_on ? "ON " : "OFF");
    printf("\x1b[8;1H Sent:    %lu   Errors: %lu      ", sent_count, err_count);
    printf("\x1b[9;1H Last:    %-40.40s", last_msg);
    printf("\x1b[12;1H \x1b[33mControls\x1b[0m");
    printf("\x1b[13;1H  Tabs (bottom):  XY | FADERS | PADS | MOTION");
    printf("\x1b[14;1H  Buttons, D-pad, L/R/ZL/ZR, sticks all send");
    printf("\x1b[16;1H  SELECT  set target IP / port");
    printf("\x1b[17;1H  START   quit");
    if (err_count > 0)
        printf("\x1b[20;1H \x1b[31mSend errors: is Wi-Fi on + IP right?\x1b[0m");
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);

    HIDUSER_EnableAccelerometer();
    HIDUSER_EnableGyroscope();

    config_load();

    u32 *soc_buf = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    bool net_ok = soc_buf && R_SUCCEEDED(socInit(soc_buf, SOC_BUFFERSIZE));
    if (net_ok) sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (!net_ok || sock < 0) {
        printf("\x1b[31mNetwork init failed.\x1b[0m\nPress START to exit.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        if (soc_buf) free(soc_buf);
        gfxExit();
        return 0;
    }
    net_set_dest();
    send_f("/hello", 1, 1, 0, 0);

    u32 frame_no = 0;
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown(), kHeld = hidKeysHeld(), kUp = hidKeysUp();

        if (kDown & KEY_START) break;
        if (kDown & KEY_SELECT) edit_settings();

        handle_buttons(kDown, kUp);
        handle_sticks();
        handle_touch(kDown, kHeld, kUp);
        handle_motion(frame_no++);

        fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        draw_surface();
        draw_status();

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    close(sock);
    socExit();
    free(soc_buf);
    HIDUSER_DisableGyroscope();
    HIDUSER_DisableAccelerometer();
    gfxExit();
    return 0;
}
