#include "settings.h"
#include "netcfg.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define SETTINGS_PATH "sdmc:/3ds/osc3ds/eucseq.ini"

static bool osc_on = false;         // OSC output is OFF until the user turns it on
static bool gyro_on = false;
static bool open_ = false;
static unsigned frame = 0;

bool settings_osc_on(void)  { return osc_on; }
bool settings_gyro_on(void) { return gyro_on; }
bool settings_is_open(void) { return open_; }
void settings_open(void)    { open_ = true; }
void settings_close(void)   { open_ = false; }

// The HID service counts enable/disable requests, so calling Disable before
// any Enable leaves the sensors off even after a later Enable. Turn them on
// exactly once at startup and off once at exit; the GYRO toggle only decides
// whether readings are sent.
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
    mkdir("sdmc:/3ds/osc3ds", 0777);
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "# eucseq settings\nosc=%d\ngyro=%d\n", osc_on, gyro_on);
    fclose(f);
}

void settings_load(void) {
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (f) {
        char line[64];
        int v;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "osc=%d", &v) == 1) osc_on = v != 0;
            else if (sscanf(line, "gyro=%d", &v) == 1) gyro_on = v != 0;
        }
        fclose(f);
    }
    sensors_start();
}

// ~30 Hz motion over OSC, only while both OSC output and gyro are on.
void settings_tick(void) {
    frame++;
    if (!osc_on || !gyro_on || (frame & 1)) return;
    accelVector a;
    angularRate g;
    hidAccelRead(&a);
    hidGyroRead(&g);
    net_send("/gyro", "fff", g.x / 100.0, g.y / 100.0, g.z / 100.0);
    net_send("/accel", "fff", a.x / 512.0, a.y / 512.0, a.z / 512.0);   // ~1.0 = 1 g
}

// ----------------------------------------------------------------- layout ---

#define SX      30
#define SW      260
#define ROW_H   26
#define TITLE_H 28
#define CTRL_X  (SX + 132)
#define CTRL_W  (SW - 132 - 10)
enum { ROW_OSC, ROW_IP, ROW_PORT, ROW_GYRO, ROW_LOCAL, NROWS };

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

void settings_draw(void) {
    if (!open_) return;
    float y0 = pop_y(), h = pop_h();
    C2D_DrawRectSolid(0, 0, 0.5f, BOT_W, 240, C2D_Color32(0, 0, 0, 150));
    C2D_DrawRectSolid(SX, y0, 0.5f, SW, h, COL_PANEL);
    rect_outline(SX, y0, SW, h, COL_LINE);
    draw_gear(SX + 16, y0 + 14, osc_on ? COL_ACCENT : COL_FG, COL_PANEL);
    text(SX + 30, y0 + 5, 0.5f, COL_FG, 0, "SETTINGS");

    char port[8];
    snprintf(port, sizeof port, "%d", net_cfg.port);
    const char *labels[NROWS] = { "OSC OUTPUT", "TARGET IP", "PORT", "GYRO OSC", "THIS 3DS" };
    for (int r = 0; r < NROWS; r++) {
        float y = row_y(r);
        if (r > 0) C2D_DrawRectSolid(SX + 8, y, 0.5f, SW - 16, 1, C2D_Color32(38, 46, 55, 255));
        bool dim_row = (r == ROW_GYRO || r == ROW_IP || r == ROW_PORT) && !osc_on;
        text(SX + 10, y + (r == ROW_GYRO ? 3 : 6), 0.42f, dim_row ? COL_DIM : COL_FG, 0, "%s", labels[r]);
        switch (r) {
        case ROW_OSC:   toggle_seg(y, osc_on, true); break;
        case ROW_IP:    field(y, net_cfg.host, osc_on); break;
        case ROW_PORT:  field(y, port, osc_on); break;
        case ROW_GYRO:
            toggle_seg(y, gyro_on, osc_on);
            if (!osc_on) text(SX + 10, y + 15, 0.3f, COL_DIM, 0, "needs OSC output on");
            else {                                   // live readout, to check the sensors
                accelVector a;
                angularRate g;
                hidAccelRead(&a);
                hidGyroRead(&g);
                text(SX + 10, y + 15, 0.28f, gyro_on ? COL_ACCENT : COL_DIM, 0, "g %d %d %d  a %.1f %.1f %.1f",
                     g.x, g.y, g.z, a.x / 512.0, a.y / 512.0, a.z / 512.0);
            }
            break;
        case ROW_LOCAL: text(CTRL_X + CTRL_W - 2, y + 6, 0.4f, COL_DIM, C2D_AlignRight, "%s", net_local_ip()); break;
        }
    }
    float cy = close_y();
    C2D_DrawRectSolid(SX + SW - 10 - 84, cy, 0.5f, 84, 24, COL_BTN);
    text(SX + SW - 10 - 42, cy + 5, 0.42f, COL_FG, C2D_AlignCenter, "CLOSE");
    text(SX + 10, cy + 6, 0.32f, COL_DIM, 0, "OSC messages: /shape_N/pulse");
}

void settings_touch_down(int x, int y) {
    float y0 = pop_y();
    if (x < SX || x >= SX + SW || y < y0 || y >= y0 + pop_h()) { open_ = false; return; }
    float cy = close_y();
    if (y >= cy && y < cy + 24 && x >= SX + SW - 10 - 84) { open_ = false; return; }
    for (int r = 0; r < NROWS; r++) {
        float ry = row_y(r);
        if (y < ry || y >= ry + ROW_H || x < CTRL_X) continue;
        bool right_half = x >= CTRL_X + CTRL_W / 2.0f;
        switch (r) {
        case ROW_OSC:  osc_on = right_half; save(); break;
        case ROW_GYRO: gyro_on = right_half; save(); break;
        case ROW_IP:   net_edit_host(); break;     // system keyboard
        case ROW_PORT: net_edit_port(); break;
        }
        return;
    }
}
