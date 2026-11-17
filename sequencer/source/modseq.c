#include "modseq.h"
#include "netcfg.h"
#include "synth.h"
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ModSeq mod_seqs[MOD_SEQS];

// ----------------------------------------------------------------- engine ---

void mod_defaults(ModSeq out[MOD_SEQS]) {
    for (int i = 0; i < MOD_SEQS; i++) {
        ModSeq *m = &out[i];
        memset(m, 0, sizeof *m);
        m->trig = i % 5;
        m->nsteps = 8;
        m->min = 0;
        m->max = 1;
        for (int s = 0; s < MOD_MAX_STEPS; s++) m->v[s] = 0.5f;
        m->cur = -1;
    }
}

void mod_init(void) { mod_defaults(mod_seqs); }

void mod_reset(void) {
    for (int i = 0; i < MOD_SEQS; i++) mod_seqs[i].cur = -1;
    syn_mod_reset();
}

float mod_seq_value(const ModSeq *m, int step) {
    float v = m->v[step < 0 ? 0 : step % MOD_MAX_STEPS];
    return m->min + (m->max - m->min) * v;
}

void mod_on_pulse(int track) {
    for (int i = 0; i < MOD_SEQS; i++) {
        ModSeq *m = &mod_seqs[i];
        if (m->trig != track || m->nsteps < 1) continue;
        int c = m->cur + 1;
        if (c >= m->nsteps) c = 0;
        m->cur = c;
        syn_mod_target(i, mod_seq_value(m, c), m->slew_ms, m->slew_curve);
    }
}

void mod_set_all(const ModSeq src[MOD_SEQS]) {
    for (int i = 0; i < MOD_SEQS; i++) {
        mod_seqs[i] = src[i];
        ModSeq *m = &mod_seqs[i];
        if (m->nsteps < 1) m->nsteps = 1;
        if (m->nsteps > MOD_MAX_STEPS) m->nsteps = MOD_MAX_STEPS;
        if (m->trig < 0 || m->trig > 4) m->trig = 0;
        if (!(m->slew_ms >= 0)) m->slew_ms = 0;          // also catches NaN
        if (m->slew_ms > MOD_MAX_SLEW) m->slew_ms = MOD_MAX_SLEW;
        m->slew_curve = m->slew_curve == SLEW_LOG ? SLEW_LOG : SLEW_LIN;
        m->cur = -1;
    }
}

int mod_seq_targets(int seq) {
    int c = 0;
    for (int n = 0; n < SYN_MAX_NODES; n++) {
        if (!syn_nodes[n].used) continue;
        for (int k = 0; k < syn_classes[syn_nodes[n].type].nparams; k++) c += syn_nodes[n].mod_src[k] == seq;
    }
    return c;
}

// --------------------------------------------------------------------- UI ---

#define R1_Y   25          // S1..S8 tabs
#define R2_Y   48          // trigger shape, step count
#define R3_Y   71          // min / max, readout
#define R4_Y   92          // what it drives
#define ROW_H  19
#define SL_X   4
#define SL_Y   114
#define SL_W   312
#define SL_H   86
#define TAB_W  37
#define TAB_GAP 2
#define MIN_X  30
#define MAX_X  120
#define FIELD_W 58
#define SLEW_X 210          // slew time field
#define SLEW_W 50
#define CURVE_X 264         // LIN / LOG toggle
#define CURVE_W 52

static int  sel = 0;
static int  shown_step = -1;
static bool drawing = false;
static int  last_step = -1;

static bool in_rect(int x, int y, float rx, float ry, float rw, float rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void fmt_num(float v, char *out, size_t n) {
    float a = fabsf(v);
    if (a >= 100) snprintf(out, n, "%.0f", v);
    else if (a >= 10) snprintf(out, n, "%.1f", v);
    else snprintf(out, n, "%.2f", v);
}

static int slider_at(int x, const ModSeq *m) {
    float w = SL_W / (float)m->nsteps;
    int s = (int)((x - SL_X) / w);
    return s < 0 ? 0 : s >= m->nsteps ? m->nsteps - 1 : s;
}

static void set_slider(ModSeq *m, int x, int y) {
    int s = slider_at(x, m);
    float v = 1.0f - (y - SL_Y) / (float)SL_H;
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    if (last_step < 0 || last_step == s) m->v[s] = v;
    else {                                       // fill sliders skipped by a fast drag
        float v0 = m->v[last_step];
        int dir = s > last_step ? 1 : -1;
        for (int j = last_step + dir; ; j += dir) {
            m->v[j] = v0 + (v - v0) * (float)(j - last_step) / (s - last_step);
            if (j == s) break;
        }
    }
    last_step = s;
    shown_step = s;
}

void mod_page_touch(bool down, bool held, bool up, int x, int y) {
    ModSeq *m = &mod_seqs[sel];
    if (down) {
        drawing = false; last_step = -1;
        if (in_rect(x, y, 0, R1_Y, BOT_W, ROW_H + 2)) {
            int t = (x - 4) / (TAB_W + TAB_GAP);
            if (t >= 0 && t < MOD_SEQS) { sel = t; shown_step = -1; }
            return;
        }
        if (in_rect(x, y, 0, R2_Y, BOT_W, ROW_H + 2)) {
            for (int t = 0; t < 5; t++)
                if (in_rect(x, y, 40 + t * 22, R2_Y, 20, ROW_H + 2)) { m->trig = t; return; }
            int d = in_rect(x, y, 240, R2_Y, 22, ROW_H) ? -1 : in_rect(x, y, 294, R2_Y, 22, ROW_H) ? 1 : 0;
            if (d) {
                m->nsteps += d;
                if (m->nsteps < 1) m->nsteps = 1;
                if (m->nsteps > MOD_MAX_STEPS) m->nsteps = MOD_MAX_STEPS;
                if (m->cur >= m->nsteps) m->cur = -1;
            }
            return;
        }
        if (in_rect(x, y, 0, R3_Y, BOT_W, ROW_H + 2)) {          // type MIN / MAX on the keypad
            float v;
            if (in_rect(x, y, MIN_X, R3_Y, FIELD_W, ROW_H) && keyboard_number("MIN value", m->min, &v)) m->min = v;
            if (in_rect(x, y, MAX_X, R3_Y, FIELD_W, ROW_H) && keyboard_number("MAX value", m->max, &v)) m->max = v;
            if (in_rect(x, y, SLEW_X, R3_Y, SLEW_W, ROW_H) && keyboard_number("SLEW (ms, 0 = jump)", m->slew_ms, &v))
                m->slew_ms = v < 0 ? 0 : v > MOD_MAX_SLEW ? MOD_MAX_SLEW : v;
            if (in_rect(x, y, CURVE_X, R3_Y, CURVE_W, ROW_H)) m->slew_curve = m->slew_curve == SLEW_LOG ? SLEW_LIN : SLEW_LOG;
            return;
        }
        if (in_rect(x, y, SL_X, SL_Y - 4, SL_W, SL_H + 8)) drawing = true;
    }
    if ((down || held) && drawing) set_slider(m, x, y);
    if (up) drawing = false;
}

static void small_btn(float x, float y, float w, const char *label) {
    C2D_DrawRectSolid(x, y, 0.5f, w, ROW_H, COL_BTN);
    text(x + w / 2, y + 2, 0.4f, COL_FG, C2D_AlignCenter, "%s", label);
}

static void field_text(float x, float y, float w, const char *s) {
    C2D_DrawRectSolid(x, y, 0.5f, w, ROW_H, C2D_Color32(14, 17, 21, 255));
    rect_outline(x, y, w, ROW_H, COL_LINE);
    text(x + w - 5, y + 2, 0.4f, COL_FG, C2D_AlignRight, "%s", s);
}

static void field(float x, float y, float v) {
    char s[24];
    fmt_num(v, s, sizeof s);
    field_text(x, y, FIELD_W, s);
}

void mod_page_draw(void) {
    ModSeq *m = &mod_seqs[sel];
    int ntargets = mod_seq_targets(sel);

    // R1: S1..S8
    for (int i = 0; i < MOD_SEQS; i++) {
        float x = 4 + i * (TAB_W + TAB_GAP);
        bool on = i == sel, used = mod_seq_targets(i) > 0;
        C2D_DrawRectSolid(x, R1_Y, 0.5f, TAB_W, ROW_H + 2, on ? COL_ACCENT : COL_BTN);
        text(x + TAB_W / 2, R1_Y + 3, 0.42f, on || used ? COL_FG : COL_DIM, C2D_AlignCenter, "S%d", i + 1);
        if (used && !on) C2D_DrawRectSolid(x + TAB_W - 6, R1_Y + 3, 0.5f, 3, 3, track_color(mod_seqs[i].trig, 0, false));
    }

    // R2: trigger shape + step count
    text(4, R2_Y + 3, 0.36f, COL_DIM, 0, "TRIG");
    for (int t = 0; t < 5; t++) {
        bool on = t == m->trig;
        C2D_DrawRectSolid(40 + t * 22, R2_Y + 1, 0.5f, 18, ROW_H, track_color(t, 0, !on));
        if (on) rect_outline(39 + t * 22, R2_Y, 20, ROW_H + 2, COL_FG);
        text(40 + t * 22 + 9, R2_Y + 2, 0.36f, on ? C2D_Color32(10, 12, 16, 255) : COL_DIM, C2D_AlignCenter, "%d", t + 1);
    }
    text(200, R2_Y + 3, 0.36f, COL_DIM, 0, "STEPS");
    small_btn(240, R2_Y, 22, "-");
    text(278, R2_Y + 2, 0.42f, COL_FG, C2D_AlignCenter, "%d", m->nsteps);
    small_btn(294, R2_Y, 22, "+");

    // R3: MIN / MAX / SLEW (tap to type) + curve toggle
    text(4, R3_Y + 3, 0.36f, COL_DIM, 0, "MIN");
    field(MIN_X, R3_Y, m->min);
    text(MAX_X - 25, R3_Y + 3, 0.36f, COL_DIM, 0, "MAX");
    field(MAX_X, R3_Y, m->max);
    text(SLEW_X - 29, R3_Y + 3, 0.36f, COL_DIM, 0, "SLEW");
    char sl[16];
    if (m->slew_ms >= 1000) snprintf(sl, sizeof sl, "%.1fs", m->slew_ms / 1000.0f);
    else snprintf(sl, sizeof sl, "%dms", (int)lroundf(m->slew_ms));
    field_text(SLEW_X, R3_Y, SLEW_W, sl);
    {   // LIN / LOG, with a tiny picture of the curve
        bool on = m->slew_ms > 0;
        C2D_DrawRectSolid(CURVE_X, R3_Y, 0.5f, CURVE_W, ROW_H, COL_BTN);
        text(CURVE_X + 4, R3_Y + 2, 0.4f, on ? COL_FG : COL_DIM, 0, "%s", m->slew_curve == SLEW_LOG ? "LOG" : "LIN");
        float gx = CURVE_X + 32, gy = R3_Y + ROW_H - 4, gw = 16, gh = ROW_H - 8;
        u32 col = on ? COL_ACCENT : COL_DIM;
        for (int i = 0; i < 8; i++) {
            float t0 = i / 8.0f, t1 = (i + 1) / 8.0f;
            C2D_DrawLine(gx + gw * t0, gy - gh * syn_slew_shape(m->slew_curve, t0), col,
                         gx + gw * t1, gy - gh * syn_slew_shape(m->slew_curve, t1), col, 1.2f, 0.5f);
        }
    }

    // R4: what this sequencer drives, and the step being shown
    int rs = shown_step >= 0 && shown_step < m->nsteps ? shown_step : m->cur;
    if (rs >= 0) {
        char v[24];
        fmt_num(mod_seq_value(m, rs), v, sizeof v);
        text(BOT_W - 6, R4_Y + 3, 0.36f, COL_FG, C2D_AlignRight, "step %d = %s", rs + 1, v);
    }
    if (!ntargets) {
        text(4, R4_Y + 3, 0.32f, COL_DIM, 0, rs >= 0 ? "Not assigned: tap an S box in a node's settings."
                                                    : "Not assigned. Tap a parameter's S box in a SYNTH node's settings.");
    } else {
        char list[160] = "drives ";
        int shown = 0;
        for (int n = 0; n < SYN_MAX_NODES; n++) {
            const SynNode *node = &syn_nodes[n];
            if (!node->used) continue;
            const NodeClass *c = &syn_classes[node->type];
            for (int k = 0; k < c->nparams; k++) {
                if (node->mod_src[k] != sel) continue;
                char item[32];
                snprintf(item, sizeof item, "%s%s %d %s", shown ? ", " : "", c->name, n + 1, c->params[k].name);
                if (strlen(list) + strlen(item) < (rs >= 0 ? 44u : 60u)) strncat(list, item, sizeof list - strlen(list) - 1);
                else if (!strstr(list, "...")) strncat(list, ", ...", sizeof list - strlen(list) - 1);
                shown++;
            }
        }
        text(4, R4_Y + 3, 0.34f, COL_ACCENT, 0, "%s", list);
    }

    // Sliders
    float gap = m->nsteps > 12 ? 1 : 2;
    float w = SL_W / (float)m->nsteps;
    for (int s = 0; s < m->nsteps; s++) {
        float x = SL_X + s * w;
        bool playing = s == m->cur;
        C2D_DrawRectSolid(x, SL_Y, 0.5f, w - gap, SL_H, C2D_Color32(14, 17, 21, 255));
        float h = m->v[s] * SL_H;
        u32 fill = playing ? track_color(m->trig, 0.35f, false) : track_color(m->trig, 0, false);
        C2D_DrawRectSolid(x, SL_Y + SL_H - h, 0.5f, w - gap, h < 1 ? 1 : h, fill);
        if (playing) rect_outline(x, SL_Y, w - gap, SL_H, COL_FG);
        if (w >= 14) text(x + (w - gap) / 2, SL_Y + SL_H - 12, 0.28f, h > 14 ? C2D_Color32(10, 12, 16, 255) : COL_DIM,
                          C2D_AlignCenter, "%d", s + 1);
    }
}
