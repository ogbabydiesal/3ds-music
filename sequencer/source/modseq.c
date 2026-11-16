#include "modseq.h"
#include "synth.h"
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ModLane mod_lanes[MOD_LANES];

// ----------------------------------------------------------------- engine ---

void mod_defaults(ModLane out[MOD_LANES]) {
    for (int l = 0; l < MOD_LANES; l++) {
        ModLane *L = &out[l];
        memset(L, 0, sizeof *L);
        L->node = -1;
        L->trig = l < 5 ? l : 0;
        L->nsteps = 8;
        L->min_u = 0.0f;
        L->max_u = 1.0f;
        for (int s = 0; s < MOD_MAX_STEPS; s++) L->v[s] = 0.5f;
        L->cur = -1;
    }
}

void mod_init(void) { mod_defaults(mod_lanes); }

void mod_reset(void) {
    for (int l = 0; l < MOD_LANES; l++) mod_lanes[l].cur = -1;
}

bool mod_lane_valid(const ModLane *L) {
    if (L->node < 0 || L->node >= SYN_MAX_NODES || !syn_nodes[L->node].used) return false;
    return L->param >= 0 && L->param < syn_classes[syn_nodes[L->node].type].nparams;
}

static const ParamDef *lane_def(const ModLane *L) {
    return &syn_classes[syn_nodes[L->node].type].params[L->param];
}

float mod_lane_value(const ModLane *L, int step) {
    float v = L->v[step < 0 ? 0 : step % MOD_MAX_STEPS];
    return syn_norm_to_param(lane_def(L), L->min_u + (L->max_u - L->min_u) * v);
}

void mod_on_pulse(int track) {
    for (int l = 0; l < MOD_LANES; l++) {
        ModLane *L = &mod_lanes[l];
        if (L->trig != track || !mod_lane_valid(L) || L->nsteps < 1) continue;
        int c = L->cur + 1;
        if (c >= L->nsteps) c = 0;
        L->cur = c;
        syn_set_param(L->node, L->param, mod_lane_value(L, c));
    }
}

void mod_set_all(const ModLane src[MOD_LANES]) {
    for (int l = 0; l < MOD_LANES; l++) {
        mod_lanes[l] = src[l];
        ModLane *L = &mod_lanes[l];
        if (L->nsteps < 1) L->nsteps = 1;
        if (L->nsteps > MOD_MAX_STEPS) L->nsteps = MOD_MAX_STEPS;
        if (L->trig < 0 || L->trig > 4) L->trig = 0;
        L->cur = -1;
    }
}

// --------------------------------------------------------------------- UI ---

#define R1_Y   25          // lane tabs + trigger shape
#define R2_Y   48          // target node / parameter
#define R3_Y   71          // min / max
#define R4_Y   92          // steps count + value readout
#define ROW_H  19
#define SL_X   4
#define SL_Y   114
#define SL_W   312
#define SL_H   86

static int  lane = 0;
static int  shown_step = -1;        // last slider touched (for the readout)
enum { DRAG_NONE, DRAG_SLIDERS, DRAG_MIN, DRAG_MAX };
static int  drag = DRAG_NONE, last_step = -1;

static bool in_rect(int x, int y, float rx, float ry, float rw, float rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void format_param(const ParamDef *d, float v, char *out, size_t n) {
    if (d->kind == PARAM_ENUM) snprintf(out, n, "%s", d->labels[(int)v]);
    else if (d->unit[0] == '%') snprintf(out, n, "%d%%", (int)lroundf(v * 100));
    else if (v < 10) snprintf(out, n, "%.1f %s", v, d->unit);
    else snprintf(out, n, "%d %s", (int)lroundf(v), d->unit);
}

// Next/previous used node (wrapping through "no target")
static int step_node(int from, int dir) {
    int i = from;
    for (int k = 0; k <= SYN_MAX_NODES; k++) {
        i += dir;
        if (i >= SYN_MAX_NODES) i = -1;
        if (i < -1) i = SYN_MAX_NODES - 1;
        if (i == -1 || syn_nodes[i].used) return i;
    }
    return -1;
}

static int slider_at(int x, const ModLane *L) {
    float w = SL_W / (float)L->nsteps;
    int s = (int)((x - SL_X) / w);
    return s < 0 ? 0 : s >= L->nsteps ? L->nsteps - 1 : s;
}

static void set_slider(ModLane *L, int x, int y) {
    int s = slider_at(x, L);
    float v = 1.0f - (y - SL_Y) / (float)SL_H;
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    if (last_step < 0 || last_step == s) L->v[s] = v;
    else {                                       // fill sliders skipped by a fast drag
        float v0 = L->v[last_step];
        int dir = s > last_step ? 1 : -1;
        for (int j = last_step + dir; ; j += dir) {
            L->v[j] = v0 + (v - v0) * (float)(j - last_step) / (s - last_step);
            if (j == s) break;
        }
    }
    last_step = s;
    shown_step = s;
}

#define BAR_W 66
static const float min_bar_x = 32, max_bar_x = 186;

void mod_page_touch(bool down, bool held, bool up, int x, int y) {
    ModLane *L = &mod_lanes[lane];
    if (down) {
        drag = DRAG_NONE; last_step = -1;
        if (in_rect(x, y, 0, R1_Y, BOT_W, ROW_H + 2)) {
            if (x < 4 + MOD_LANES * 30) { lane = (x - 4) / 30; shown_step = -1; return; }
            for (int t = 0; t < 5; t++)
                if (in_rect(x, y, 168 + t * 22, R1_Y, 20, ROW_H + 2)) { L->trig = t; return; }
            return;
        }
        if (in_rect(x, y, 0, R2_Y, BOT_W, ROW_H + 2)) {
            if (x < 24)             { L->node = step_node(L->node, -1); L->param = 0; }
            else if (x >= 120 && x < 144) { L->node = step_node(L->node, +1); L->param = 0; }
            else if (mod_lane_valid(L)) {
                int np = syn_classes[syn_nodes[L->node].type].nparams;
                if (x >= 148 && x < 170) L->param = (L->param + np - 1) % np;
                else if (x >= 294)       L->param = (L->param + 1) % np;
            }
            L->cur = -1;
            return;
        }
        if (in_rect(x, y, 0, R3_Y, BOT_W, ROW_H + 2)) {
            if (x >= min_bar_x - 4 && x < min_bar_x + BAR_W + 4) drag = DRAG_MIN;
            else if (x >= max_bar_x - 4 && x < max_bar_x + BAR_W + 4) drag = DRAG_MAX;
        } else if (in_rect(x, y, 0, R4_Y, BOT_W, ROW_H + 2)) {
            int d = in_rect(x, y, 44, R4_Y, 22, ROW_H) ? -1 : in_rect(x, y, 92, R4_Y, 22, ROW_H) ? 1 : 0;
            if (d) {
                L->nsteps += d;
                if (L->nsteps < 1) L->nsteps = 1;
                if (L->nsteps > MOD_MAX_STEPS) L->nsteps = MOD_MAX_STEPS;
                if (L->cur >= L->nsteps) L->cur = -1;
            }
            return;
        } else if (in_rect(x, y, SL_X, SL_Y - 4, SL_W, SL_H + 8)) {
            drag = DRAG_SLIDERS;
        }
    }
    if ((down || held) && drag != DRAG_NONE) {
        if (drag == DRAG_SLIDERS) set_slider(L, x, y);
        else {
            float bx = drag == DRAG_MIN ? min_bar_x : max_bar_x;
            float u = (x - bx) / BAR_W;
            u = u < 0 ? 0 : u > 1 ? 1 : u;
            if (drag == DRAG_MIN) L->min_u = u; else L->max_u = u;
        }
    }
    if (up) drag = DRAG_NONE;
}

static void small_btn(float x, float y, float w, const char *label, bool enabled) {
    C2D_DrawRectSolid(x, y, 0.5f, w, ROW_H, enabled ? COL_BTN : COL_PANEL);
    text(x + w / 2, y + 2, 0.4f, enabled ? COL_FG : COL_DIM, C2D_AlignCenter, "%s", label);
}

void mod_page_draw(void) {
    ModLane *L = &mod_lanes[lane];
    bool valid = mod_lane_valid(L);
    u32 tcol = track_color(L->trig, 0, false);

    // R1: lane tabs + trigger shape
    for (int l = 0; l < MOD_LANES; l++) {
        bool sel = l == lane, active = mod_lane_valid(&mod_lanes[l]);
        C2D_DrawRectSolid(4 + l * 30, R1_Y, 0.5f, 28, ROW_H + 2, sel ? COL_ACCENT : COL_BTN);
        text(4 + l * 30 + 14, R1_Y + 2, 0.45f, sel || active ? COL_FG : COL_DIM, C2D_AlignCenter, "%c", 'A' + l);
        if (active && !sel) C2D_DrawRectSolid(4 + l * 30 + 22, R1_Y + 3, 0.5f, 3, 3, track_color(mod_lanes[l].trig, 0, false));
    }
    text(132, R1_Y + 4, 0.36f, COL_DIM, 0, "TRIG");
    for (int t = 0; t < 5; t++) {
        bool on = t == L->trig;
        C2D_DrawRectSolid(168 + t * 22, R1_Y + 1, 0.5f, 18, ROW_H, track_color(t, 0, !on));
        if (on) rect_outline(167 + t * 22, R1_Y, 20, ROW_H + 2, COL_FG);
        text(168 + t * 22 + 9, R1_Y + 2, 0.36f, on ? C2D_Color32(10, 12, 16, 255) : COL_DIM, C2D_AlignCenter, "%d", t + 1);
    }

    // R2: target node + parameter
    small_btn(4, R2_Y, 20, "<", true);
    small_btn(120, R2_Y, 24, ">", true);
    C2D_DrawRectSolid(26, R2_Y, 0.5f, 92, ROW_H, C2D_Color32(14, 17, 21, 255));
    if (L->node >= 0 && L->node < SYN_MAX_NODES && syn_nodes[L->node].used)
        text(72, R2_Y + 2, 0.4f, COL_FG, C2D_AlignCenter, "%s %d", syn_classes[syn_nodes[L->node].type].name, L->node + 1);
    else text(72, R2_Y + 3, 0.36f, COL_DIM, C2D_AlignCenter, "no target");
    small_btn(148, R2_Y, 22, "<", valid);
    small_btn(294, R2_Y, 22, ">", valid);
    C2D_DrawRectSolid(172, R2_Y, 0.5f, 120, ROW_H, C2D_Color32(14, 17, 21, 255));
    text(232, R2_Y + 2, 0.4f, valid ? COL_FG : COL_DIM, C2D_AlignCenter, "%s", valid ? lane_def(L)->name : "-");

    // R3: min / max (as positions on the parameter's scale) with values
    const char *lbl[2] = { "MIN", "MAX" };
    float bxs[2] = { min_bar_x, max_bar_x }, us[2] = { L->min_u, L->max_u };
    for (int k = 0; k < 2; k++) {
        text(bxs[k] - 28, R3_Y + 3, 0.36f, COL_DIM, 0, "%s", lbl[k]);
        C2D_DrawRectSolid(bxs[k], R3_Y + 6, 0.5f, BAR_W, 7, COL_BTN);
        C2D_DrawRectSolid(bxs[k], R3_Y + 6, 0.5f, BAR_W * us[k], 7, valid ? COL_ACCENT : COL_LINE);
        char v[24] = "-";
        if (valid) format_param(lane_def(L), syn_norm_to_param(lane_def(L), us[k]), v, sizeof v);
        text(bxs[k] + BAR_W + 4, R3_Y + 3, 0.34f, valid ? COL_FG : COL_DIM, 0, "%s", v);
    }

    // R4: step count + readout of the touched (or playing) step
    text(4, R4_Y + 3, 0.36f, COL_DIM, 0, "STEPS");
    small_btn(44, R4_Y, 22, "-", true);
    text(79, R4_Y + 2, 0.42f, COL_FG, C2D_AlignCenter, "%d", L->nsteps);
    small_btn(92, R4_Y, 22, "+", true);
    int rs = shown_step >= 0 && shown_step < L->nsteps ? shown_step : L->cur;
    if (valid && rs >= 0) {
        char v[24];
        format_param(lane_def(L), mod_lane_value(L, rs), v, sizeof v);
        text(BOT_W - 6, R4_Y + 3, 0.36f, COL_FG, C2D_AlignRight, "step %d = %s", rs + 1, v);
    } else if (!valid) {
        text(BOT_W - 6, R4_Y + 3, 0.32f, COL_DIM, C2D_AlignRight, "pick a TARGET to use this lane");
    }

    // Sliders
    float gap = L->nsteps > 12 ? 1 : 2;
    float w = SL_W / (float)L->nsteps;
    for (int s = 0; s < L->nsteps; s++) {
        float x = SL_X + s * w;
        bool playing = s == L->cur;
        C2D_DrawRectSolid(x, SL_Y, 0.5f, w - gap, SL_H, C2D_Color32(14, 17, 21, 255));
        float h = L->v[s] * SL_H;
        u32 fill = !valid ? COL_LINE : playing ? track_color(L->trig, 0.35f, false) : track_color(L->trig, 0, false);
        C2D_DrawRectSolid(x, SL_Y + SL_H - h, 0.5f, w - gap, h < 1 ? 1 : h, fill);
        if (playing && valid) rect_outline(x, SL_Y, w - gap, SL_H, COL_FG);
        if (w >= 14) text(x + (w - gap) / 2, SL_Y + SL_H - 12, 0.28f, h > 14 ? C2D_Color32(10, 12, 16, 255) : COL_DIM,
                          C2D_AlignCenter, "%d", s + 1);
    }
    (void)tcol;
}
