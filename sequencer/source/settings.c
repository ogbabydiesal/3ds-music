#include "settings.h"
#include "netcfg.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define SETTINGS_PATH     "sdmc:/3ds/nodeTrix/settings.ini"
#define OLD_SETTINGS_PATH "sdmc:/3ds/osc3ds/eucseq.ini"     // before the eucseq -> nodeTrix rename

static bool osc_on = false;         // OSC output is OFF until the user turns it on
static bool gyro_on = false;
static bool accel_on = false;
static bool open_ = false;
static bool credits_ = false;              // showing the CREDITS page
static unsigned frame = 0;

bool settings_osc_on(void)  { return osc_on; }
bool settings_gyro_on(void) { return gyro_on; }
bool settings_accel_on(void) { return accel_on; }
bool settings_is_open(void) { return open_; }
void settings_open(void)    { open_ = true; credits_ = false; }
void settings_close(void)   { open_ = false; credits_ = false; }

// The HID service counts enable/disable requests, so calling Disable before
// any Enable leaves the sensors off even after a later Enable. Turn them on
// exactly once at startup and off once at exit; the GYRO / ACCEL toggles only
// decide whether readings are sent.
static bool sensors_on = false;

static void sensors_start(void) {
    if (sensors_on) return;
    HIDUSER_EnableAccelerometer();
    HIDUSER_EnableGyroscope();
    sensors_on = true;
}

void settings_exit(void) {
    if (!sensors_on) return;
    HIDUSER_DisableGyroscope();
    HIDUSER_DisableAccelerometer();
    sensors_on = false;
}

static void save(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/nodeTrix", 0777);
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "# nodeTrix settings\nosc=%d\ngyro=%d\naccel=%d\n", osc_on, gyro_on, accel_on);
    fclose(f);
}

void settings_load(void) {
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) f = fopen(OLD_SETTINGS_PATH, "r");
    if (f) {
        char line[64];
        int v;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "osc=%d", &v) == 1) osc_on = v != 0;
            else if (sscanf(line, "gyro=%d", &v) == 1) gyro_on = v != 0;
            else if (sscanf(line, "accel=%d", &v) == 1) accel_on = v != 0;
        }
        fclose(f);
    }
    sensors_start();
}

// ~30 Hz motion over OSC; gyro and accel each have their own switch.
void settings_tick(void) {
    frame++;
    if (!osc_on || (frame & 1)) return;
    if (gyro_on) {
        angularRate g;
        hidGyroRead(&g);
        net_send("/gyro", "fff", g.x / 100.0, g.y / 100.0, g.z / 100.0);
    }
    if (accel_on) {
        accelVector a;
        hidAccelRead(&a);
        net_send("/accel", "fff", a.x / 512.0, a.y / 512.0, a.z / 512.0);   // ~1.0 = 1 g
    }
}

// ----------------------------------------------------------------- layout ---

#define SX      30
#define SW      260
#define ROW_H   26
#define TITLE_H 28
#define CTRL_X  (SX + 132)
#define CTRL_W  (SW - 132 - 10)
enum { ROW_OSC, ROW_IP, ROW_PORT, ROW_GYRO, ROW_ACCEL, ROW_LOCAL, NROWS };

static float pop_h(void) { return TITLE_H + NROWS * ROW_H + 8 + 24 + 8; }
static float pop_y(void) { return (240 - pop_h()) / 2; }
static float row_y(int r) { return pop_y() + TITLE_H + r * ROW_H; }
static float close_y(void) { return row_y(NROWS) + 8; }

static void toggle_seg(float y, bool on, bool enabled) {
    float w = CTRL_W / 2.0f;
    for (int s = 0; s < 2; s++) {
        bool lit = (s == 1) == on;
        u32 fill = lit ? (s == 1 ? (enabled ? COL_ACCENT : COL_LINE) : COL_BTN) : C2D_Color32(24, 29, 35, 255);
        C2D_DrawRectSolid(CTRL_X + s * w + 1, y + 3, 0.5f, w - 2, ROW_H - 6, fill);
        rect_outline(CTRL_X + s * w + 1, y + 3, w - 2, ROW_H - 6, COL_LINE);
        text(CTRL_X + s * w + w / 2, y + 6, 0.4f, lit ? COL_FG : COL_DIM, C2D_AlignCenter, s ? "ON" : "OFF");
    }
}

static void field(float y, const char *value, bool enabled) {
    C2D_DrawRectSolid(CTRL_X + 1, y + 3, 0.5f, CTRL_W - 2, ROW_H - 6, C2D_Color32(14, 17, 21, 255));
    rect_outline(CTRL_X + 1, y + 3, CTRL_W - 2, ROW_H - 6, COL_LINE);
    text(CTRL_X + 8, y + 6, 0.4f, enabled ? COL_FG : COL_DIM, 0, "%s", value);
}

// Attribution for the algorithms the synth is built on (see docs/LICENSES.md).
static void credits_draw(float y0) {
    text(SX + 10, y0 + 5, 0.5f, COL_FG, 0, "CREDITS");
    static const struct { const char *head; const char *lines[3]; } cr[] = {
        { "GRAINS", { "An implementation of the Beads granular algorithm",
                      "by Emilie Gillet / Mutable Instruments, written from",
                      "its manual. Not affiliated with Mutable Instruments." } },
        { "REVERB", { "JCRev from the Synthesis ToolKit (STK), after John",
                      "Chowning. Copyright (c) 1995-2023 Perry R. Cook and",
                      "Gary P. Scavone. STK license (MIT-style)." } },
        { "NOISE",  { "Pink noise: Paul Kellett's filter (music-dsp).", NULL, NULL } },
        { "LIBRARIES", { "libctru, citro2d, citro3d (zlib); newlib.",
                         "Full notices: THIRD_PARTY_NOTICES.md", NULL } },
    };
    float y = y0 + TITLE_H;
    for (size_t i = 0; i < sizeof cr / sizeof cr[0]; i++) {
        text(SX + 10, y, 0.38f, COL_ACCENT, 0, "%s", cr[i].head);
        y += 13;
        for (int k = 0; k < 3 && cr[i].lines[k]; k++) { text(SX + 10, y, 0.31f, COL_FG, 0, "%s", cr[i].lines[k]); y += 11; }
        y += 4;
    }
    float cy = close_y();
    C2D_DrawRectSolid(SX + SW - 10 - 84, cy, 0.5f, 84, 24, COL_BTN);
    text(SX + SW - 10 - 42, cy + 5, 0.42f, COL_FG, C2D_AlignCenter, "BACK");
}

void settings_draw(void) {
    if (!open_) return;
    float y0 = pop_y(), h = pop_h();
    C2D_DrawRectSolid(0, 0, 0.5f, BOT_W, 240, C2D_Color32(0, 0, 0, 150));
    C2D_DrawRectSolid(SX, y0, 0.5f, SW, h, COL_PANEL);
    rect_outline(SX, y0, SW, h, COL_LINE);
    if (credits_) { credits_draw(y0); return; }
    draw_gear(SX + 16, y0 + 14, osc_on ? COL_ACCENT : COL_FG, COL_PANEL);
    text(SX + 30, y0 + 5, 0.5f, COL_FG, 0, "SETTINGS");

    char port[8];
    snprintf(port, sizeof port, "%d", net_cfg.port);
    const char *labels[NROWS] = { "OSC OUTPUT", "TARGET IP", "PORT", "GYRO", "ACCEL", "THIS 3DS" };
    for (int r = 0; r < NROWS; r++) {
        float y = row_y(r);
        if (r > 0) C2D_DrawRectSolid(SX + 8, y, 0.5f, SW - 16, 1, C2D_Color32(38, 46, 55, 255));
        bool motion = r == ROW_GYRO || r == ROW_ACCEL;
        bool dim_row = (motion || r == ROW_IP || r == ROW_PORT) && !osc_on;
        text(SX + 10, y + (motion ? 2 : 6), 0.42f, dim_row ? COL_DIM : COL_FG, 0, "%s", labels[r]);
        if (motion)
            text(SX + 10, y + 15, 0.3f, COL_DIM, 0, !osc_on ? "needs OSC output on"
                 : r == ROW_GYRO ? "sends /gyro x y z" : "sends /accel x y z");
        switch (r) {
        case ROW_OSC:   toggle_seg(y, osc_on, true); break;
        case ROW_IP:    field(y, net_cfg.host, osc_on); break;
        case ROW_PORT:  field(y, port, osc_on); break;
        case ROW_GYRO:  toggle_seg(y, gyro_on, osc_on); break;
        case ROW_ACCEL: toggle_seg(y, accel_on, osc_on); break;
        case ROW_LOCAL: text(CTRL_X + CTRL_W - 2, y + 6, 0.4f, COL_DIM, C2D_AlignRight, "%s", net_local_ip()); break;
        }
    }
    float cy = close_y();
    C2D_DrawRectSolid(SX + SW - 10 - 84, cy, 0.5f, 84, 24, COL_BTN);
    text(SX + SW - 10 - 42, cy + 5, 0.42f, COL_FG, C2D_AlignCenter, "CLOSE");
    C2D_DrawRectSolid(SX + 10, cy, 0.5f, 84, 24, COL_BTN);
    text(SX + 10 + 42, cy + 5, 0.42f, COL_FG, C2D_AlignCenter, "CREDITS");
}

void settings_touch_down(int x, int y) {
    float y0 = pop_y();
    if (x < SX || x >= SX + SW || y < y0 || y >= y0 + pop_h()) { open_ = false; credits_ = false; return; }
    float cy = close_y();
    if (credits_) {                                          // BACK returns to the settings
        if (y >= cy && y < cy + 24 && x >= SX + SW - 10 - 84) credits_ = false;
        return;
    }
    if (y >= cy && y < cy + 24 && x >= SX + SW - 10 - 84) { open_ = false; return; }
    if (y >= cy && y < cy + 24 && x >= SX + 10 && x < SX + 10 + 84) { credits_ = true; return; }
    for (int r = 0; r < NROWS; r++) {
        float ry = row_y(r);
        if (y < ry || y >= ry + ROW_H || x < CTRL_X) continue;
        bool right_half = x >= CTRL_X + CTRL_W / 2.0f;
        switch (r) {
        case ROW_OSC:  osc_on = right_half; save(); break;
        case ROW_GYRO:  gyro_on = right_half; save(); break;
        case ROW_ACCEL: accel_on = right_half; save(); break;
        case ROW_IP:   net_edit_host(); break;     // system keyboard
        case ROW_PORT: net_edit_port(); break;
        }
        return;
    }
}
