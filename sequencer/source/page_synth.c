#include "page_synth.h"
#include "synth.h"
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ----------------------------------------------------------------- layout ---

#define CAR_Y       25
#define CAR_H       24
#define CAR_ARROW_W 22
#define CAR_TILE_W  82
#define CAR_GAP     4
#define CAR_X0      (4 + CAR_ARROW_W + CAR_GAP)
#define CAR_VISIBLE 3

#define TB_Y        52
#define TB_H        18

#define CV_X        4
#define CV_Y        73
#define CV_W        312
#define CV_H        128

#define NODE_W      64            // world units (= pixels at 100%)
#define NODE_H      30
#define PORT        7
#define PORT_HIT    10            // screen pixels

#define POP_X       24
#define POP_W       272
#define POP_ROW_H   19
#define POP_TITLE_H 24
#define POP_BAR_X   (POP_X + 84)
#define POP_BAR_W   120
#define POP_SEG_W   (POP_W - 84 - 8)
#define WAVE_ED_H   44

// Toolbar buttons
enum { TB_MODE, TB_ZOUT, TB_FIT, TB_ZIN, TB_COPY, TB_PASTE, TB_DEL, TB_COUNT };
static const float tb_x[TB_COUNT] = { 4, 62, 86, 126, 154, 208, 264 };
static const float tb_w[TB_COUNT] = { 54, 22, 38, 22, 50, 52, 52 };

// ------------------------------------------------------------------ state ---

enum { DRAG_NONE, DRAG_NODE, DRAG_WIRE, DRAG_NEW, DRAG_PARAM, DRAG_WAVE, DRAG_BOX, DRAG_PAN };

static int   car_scroll = 0;
static int   drag = DRAG_NONE;
static int   drag_node = -1, drag_class = -1, drag_param = -1, wave_last = -1;
static bool  drag_moved = false;
static int   down_x, down_y, cur_x, cur_y, prev_x, prev_y;
static int   popup = -1;                    // node being edited, -1 = closed
static int   tb_pressed = -1;

static float zoom = 1.0f, pan_x = 0, pan_y = 0;   // world point at the canvas's top-left
static bool  pan_mode = false;                    // empty-canvas drag: false = box select
static bool  selected[SYN_MAX_NODES];

static SynNode clip[SYN_MAX_NODES];
static int   clip_input[SYN_MAX_NODES], nclip = 0, paste_count = 0;

bool synth_page_popup_open(void) { return popup >= 0; }

// --------------------------------------------------------------- helpers ---

static float sx(float wx) { return CV_X + (wx - pan_x) * zoom; }
static float sy(float wy) { return CV_Y + (wy - pan_y) * zoom; }
static float wx(float x)  { return (x - CV_X) / zoom + pan_x; }
static float wy(float y)  { return (y - CV_Y) / zoom + pan_y; }

static bool in_rect(float x, float y, float rx, float ry, float rw, float rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}
static bool in_canvas(int x, int y) { return in_rect(x, y, CV_X, CV_Y, CV_W, CV_H); }

static float node_sx(const SynNode *n) { return sx(n->x); }
static float node_sy(const SynNode *n) { return sy(n->y); }
static float port_sy(const SynNode *n) { return sy(n->y + NODE_H / 2.0f); }

static int node_at(int x, int y) {
    for (int i = SYN_MAX_NODES - 1; i >= 0; i--)
        if (syn_nodes[i].used &&
            in_rect(x, y, node_sx(&syn_nodes[i]), node_sy(&syn_nodes[i]), NODE_W * zoom, NODE_H * zoom)) return i;
    return -1;
}

static int out_port_at(int x, int y) {
    for (int i = SYN_MAX_NODES - 1; i >= 0; i--) {
        const SynNode *n = &syn_nodes[i];
        if (n->used && fabsf(x - sx(n->x + NODE_W)) <= PORT_HIT && fabsf(y - port_sy(n)) <= PORT_HIT) return i;
    }
    return -1;
}

static int in_port_at(int x, int y) {
    for (int i = SYN_MAX_NODES - 1; i >= 0; i--) {
        const SynNode *n = &syn_nodes[i];
        if (n->used && syn_classes[n->type].has_input &&
            fabsf(x - sx(n->x)) <= PORT_HIT && fabsf(y - port_sy(n)) <= PORT_HIT) return i;
    }
    return -1;
}

static void clear_selection(void) { memset(selected, 0, sizeof selected); }

static int selection_count(void) {
    int c = 0;
    for (int i = 0; i < SYN_MAX_NODES; i++) c += syn_nodes[i].used && selected[i];
    return c;
}

// First spot in the visible area that doesn't overlap a node (world coords).
static void free_spot(float *ox, float *oy) {
    float x0 = wx(CV_X) + 8, y0 = wy(CV_Y) + 8, x1 = wx(CV_X + CV_W) - NODE_W, y1 = wy(CV_Y + CV_H) - NODE_H;
    for (float gy = y0; gy <= y1; gy += NODE_H + 10)
        for (float gx = x0; gx <= x1; gx += NODE_W + 22) {
            bool clear = true;
            for (int i = 0; i < SYN_MAX_NODES && clear; i++) {
                const SynNode *n = &syn_nodes[i];
                if (n->used && fabsf(n->x - gx) < NODE_W + 8 && fabsf(n->y - gy) < NODE_H + 6) clear = false;
            }
            if (clear) { *ox = gx; *oy = gy; return; }
        }
    *ox = wx(CV_X + CV_W / 2) - NODE_W / 2;
    *oy = wy(CV_Y + CV_H / 2) - NODE_H / 2;
}

// ------------------------------------------------------------ view ops ---

static void zoom_by(float f) {
    float cx = CV_X + CV_W / 2.0f, cy = CV_Y + CV_H / 2.0f;
    float ax = wx(cx), ay = wy(cy);
    zoom *= f;
    if (zoom < 0.4f) zoom = 0.4f;
    if (zoom > 2.5f) zoom = 2.5f;
    pan_x = ax - (cx - CV_X) / zoom;
    pan_y = ay - (cy - CV_Y) / zoom;
}

static void zoom_fit(void) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < SYN_MAX_NODES; i++) {
        const SynNode *n = &syn_nodes[i];
        if (!n->used) continue;
        if (n->x < x0) x0 = n->x;
        if (n->y < y0) y0 = n->y;
        if (n->x + NODE_W > x1) x1 = n->x + NODE_W;
        if (n->y + NODE_H > y1) y1 = n->y + NODE_H;
    }
    if (x0 > x1) { zoom = 1; pan_x = pan_y = 0; return; }
    float zx = (CV_W - 24) / (x1 - x0), zy = (CV_H - 20) / (y1 - y0);
    zoom = zx < zy ? zx : zy;
    if (zoom > 1.5f) zoom = 1.5f;
    if (zoom < 0.4f) zoom = 0.4f;
    pan_x = (x0 + x1) / 2 - CV_W / 2.0f / zoom;
    pan_y = (y0 + y1) / 2 - CV_H / 2.0f / zoom;
}

// --------------------------------------------------------- copy / paste ---

static void copy_selection(void) {
    int map[SYN_MAX_NODES];
    nclip = 0;
    for (int i = 0; i < SYN_MAX_NODES; i++) {
        map[i] = -1;
        if (syn_nodes[i].used && selected[i]) { map[i] = nclip; clip[nclip++] = syn_nodes[i]; }
    }
    for (int k = 0; k < nclip; k++)                 // keep only links inside the selection
        clip_input[k] = clip[k].input >= 0 ? map[clip[k].input] : -1;
    paste_count = 0;
}

static void paste_clip(void) {
    if (!nclip) return;
    int idx[SYN_MAX_NODES];
    float off = 18.0f * ++paste_count;
    clear_selection();
    for (int k = 0; k < nclip; k++) {
        idx[k] = syn_add_copy(&clip[k], clip[k].x + off, clip[k].y + off);
        if (idx[k] >= 0) selected[idx[k]] = true;
    }
    for (int k = 0; k < nclip; k++)
        if (idx[k] >= 0 && clip_input[k] >= 0 && idx[clip_input[k]] >= 0) syn_connect(idx[clip_input[k]], idx[k]);
}

static void delete_selection(void) {
    for (int i = 0; i < SYN_MAX_NODES; i++)
        if (syn_nodes[i].used && selected[i]) {
            if (popup == i) popup = -1;
            syn_delete_node(i);
        }
    clear_selection();
}

// ------------------------------------------------------------------ popup ---

static bool has_wave_editor(int node) {
    const SynNode *n = &syn_nodes[node];
    return n->type == NODE_OSC && (int)n->p[OSC_WAVE] == WAVE_CUSTOM;
}
static float extra_h(int node) { return has_wave_editor(node) ? WAVE_ED_H + 4 : 0; }
static float popup_h(int node) {
    int n = syn_classes[syn_nodes[node].type].nparams;
    return POP_TITLE_H + n * POP_ROW_H + extra_h(node) + 6 + POP_ROW_H + 11 + 24 + 8;
}
static float popup_y(int node) { float y = (240 - popup_h(node)) / 2; return y < 0 ? 0 : y; }
static float row_y(int node, int k) { return popup_y(node) + POP_TITLE_H + k * POP_ROW_H; }
static float wave_y(int node) { return row_y(node, syn_classes[syn_nodes[node].type].nparams) + 2; }
static float out_y(int node) { return row_y(node, syn_classes[syn_nodes[node].type].nparams) + extra_h(node) + 6; }
static float buttons_y(int node) { return out_y(node) + POP_ROW_H + 11; }

#define WAVE_X  (POP_X + 8)
#define WAVE_W  (POP_W - 16)

static void set_param_from_x(int node, int k, int x) {
    const ParamDef *d = &syn_classes[syn_nodes[node].type].params[k];
    if (d->kind == PARAM_ENUM) {
        int seg = (int)((x - POP_BAR_X) / (POP_SEG_W / (float)d->nlabels));
        if (seg < 0) seg = 0;
        if (seg >= d->nlabels) seg = d->nlabels - 1;
        syn_set_param(node, k, d->min + seg);
    } else {
        syn_set_param(node, k, syn_norm_to_param(d, (x - POP_BAR_X) / (float)POP_BAR_W));
    }
}

// Draw into the custom wave; fills gaps between successive touch points.
static void wave_draw_at(int node, int x, int y) {
    int i = (int)((x - WAVE_X) / (WAVE_W / (float)SYN_WT_SIZE));
    if (i < 0) i = 0;
    if (i >= SYN_WT_SIZE) i = SYN_WT_SIZE - 1;
    float v = 1.0f - 2.0f * (y - wave_y(node)) / WAVE_ED_H;
    if (wave_last < 0 || wave_last == i) syn_set_wave_point(node, i, v);
    else {
        float v0 = syn_nodes[node].wt[wave_last];
        int step = i > wave_last ? 1 : -1;
        for (int j = wave_last + step; ; j += step) {
            float t = (float)(j - wave_last) / (i - wave_last);
            syn_set_wave_point(node, j, v0 + (v - v0) * t);
            if (j == i) break;
        }
    }
    wave_last = i;
}

static void popup_touch_down(int x, int y) {
    int node = popup;
    if (!in_rect(x, y, POP_X, popup_y(node), POP_W, popup_h(node))) { popup = -1; return; }

    const NodeClass *c = &syn_classes[syn_nodes[node].type];
    for (int k = 0; k < c->nparams; k++) {
        float ry = row_y(node, k);
        if (y < ry || y >= ry + POP_ROW_H) continue;
        float w = c->params[k].kind == PARAM_ENUM ? POP_SEG_W : POP_BAR_W;
        if (x >= POP_BAR_X - 6 && x < POP_BAR_X + w + 6) {
            set_param_from_x(node, k, x);
            if (c->params[k].kind != PARAM_ENUM) { drag = DRAG_PARAM; drag_param = k; }
        }
        return;
    }
    if (has_wave_editor(node) && in_rect(x, y, WAVE_X, wave_y(node), WAVE_W, WAVE_ED_H)) {
        drag = DRAG_WAVE; wave_last = -1;
        wave_draw_at(node, x, y);
        return;
    }
    float oy = out_y(node);
    if (y >= oy && y < oy + POP_ROW_H && x >= POP_BAR_X) {
        int seg = (int)((x - POP_BAR_X) / (POP_SEG_W / 6.0f));     // OFF + 5 shapes
        if (seg == 0) syn_clear_outputs(node);
        else if (seg > 0 && seg < 6) syn_toggle_output(node, seg - 1);
        return;
    }
    float by = buttons_y(node);
    if (y >= by && y < by + 24) {
        if (x < POP_X + 8 + 84) { syn_delete_node(node); selected[node] = false; popup = -1; }
        else if (x >= POP_X + POP_W - 8 - 84) popup = -1;
    }
}

static void format_value(const ParamDef *d, float v, char *out, size_t n) {
    if (d->unit[0] == '%') snprintf(out, n, "%d%%", (int)lroundf(v * 100));
    else if (v < 10) snprintf(out, n, "%.1f %s", v, d->unit);
    else snprintf(out, n, "%d %s", (int)lroundf(v), d->unit);
}

void synth_page_draw_popup(void) {
    if (popup < 0) return;
    int node = popup;
    const SynNode *n = &syn_nodes[node];
    const NodeClass *c = &syn_classes[n->type];
    float py = popup_y(node), ph = popup_h(node);

    C2D_DrawRectSolid(0, 0, 0.5f, BOT_W, 240, C2D_Color32(0, 0, 0, 150));
    C2D_DrawRectSolid(POP_X, py, 0.5f, POP_W, ph, COL_PANEL);
    rect_outline(POP_X, py, POP_W, ph, COL_LINE);

    text(POP_X + 8, py + 5, 0.5f, COL_FG, 0, "%s", c->name);
    if (c->has_input) text(POP_X + POP_W - 8, py + 7, 0.36f, COL_DIM, C2D_AlignRight, "input: %s", c->input_name);

    for (int k = 0; k < c->nparams; k++) {
        const ParamDef *d = &c->params[k];
        float ry = row_y(node, k);
        text(POP_X + 8, ry + 3, 0.4f, COL_DIM, 0, "%s", d->name);
        if (d->kind == PARAM_ENUM) {
            float sw = POP_SEG_W / (float)d->nlabels;
            for (int s = 0; s < d->nlabels; s++) {
                bool on = (int)n->p[k] == (int)d->min + s;
                float bx = POP_BAR_X + s * sw;
                C2D_DrawRectSolid(bx + 1, ry + 2, 0.5f, sw - 2, POP_ROW_H - 4, on ? COL_ACCENT : COL_BTN);
                text(bx + sw / 2, ry + 4, 0.34f, COL_FG, C2D_AlignCenter, "%s", d->labels[s]);
            }
        } else {
            float u = syn_param_to_norm(d, n->p[k]);
            if (u < 0) u = 0;
            if (u > 1) u = 1;
            C2D_DrawRectSolid(POP_BAR_X, ry + 6, 0.5f, POP_BAR_W, 7, COL_BTN);
            C2D_DrawRectSolid(POP_BAR_X, ry + 6, 0.5f, POP_BAR_W * u, 7, COL_ACCENT);
            char v[24];
            format_value(d, n->p[k], v, sizeof v);
            text(POP_X + POP_W - 8, ry + 3, 0.4f, COL_FG, C2D_AlignRight, "%s", v);
        }
    }

    // Custom waveform editor: 16 bars, draw with the stylus
    if (has_wave_editor(node)) {
        float y0 = wave_y(node), mid = y0 + WAVE_ED_H / 2.0f, bw = WAVE_W / (float)SYN_WT_SIZE;
        C2D_DrawRectSolid(WAVE_X, y0, 0.5f, WAVE_W, WAVE_ED_H, C2D_Color32(14, 17, 21, 255));
        rect_outline(WAVE_X, y0, WAVE_W, WAVE_ED_H, COL_PANEL2);
        C2D_DrawRectSolid(WAVE_X, mid, 0.5f, WAVE_W, 1, COL_PANEL2);
        for (int i = 0; i < SYN_WT_SIZE; i++) {
            float h = n->wt[i] * WAVE_ED_H / 2.0f;
            float top = h > 0 ? mid - h : mid;
            C2D_DrawRectSolid(WAVE_X + i * bw + 1, top, 0.5f, bw - 2, fabsf(h) < 1 ? 1 : fabsf(h), COL_ACCENT);
        }
    }

    // Output: OFF (default) or any combination of shapes
    float oy = out_y(node);
    text(POP_X + 8, oy + 3, 0.4f, COL_DIM, 0, "OUTPUT");
    float sw = POP_SEG_W / 6.0f;
    for (int s = 0; s < 6; s++) {
        float bx = POP_BAR_X + s * sw;
        bool on = s == 0 ? n->outputs == 0 : (n->outputs >> (s - 1)) & 1;
        u32 fill = !on ? C2D_Color32(24, 29, 35, 255) : (s == 0 ? COL_BTN : track_color(s - 1, 0, false));
        C2D_DrawRectSolid(bx + 2, oy + 1, 0.5f, sw - 4, POP_ROW_H - 2, fill);
        rect_outline(bx + 2, oy + 1, sw - 4, POP_ROW_H - 2, s == 0 ? COL_LINE : track_color(s - 1, 0, !on));
        text(bx + sw / 2, oy + 3, 0.36f, on ? COL_FG : COL_DIM, C2D_AlignCenter, s == 0 ? "OFF" : "%d", s);
    }
    char names[64] = "";
    for (int t = 0; t < 5; t++)
        if ((n->outputs >> t) & 1) {
            if (names[0]) strncat(names, " + ", sizeof names - strlen(names) - 1);
            strncat(names, track_names[t], sizeof names - strlen(names) - 1);
        }
    text(POP_BAR_X, oy + POP_ROW_H, 0.3f, COL_DIM, 0, "%s", names[0] ? names : "silent unless set as an output");

    float by = buttons_y(node);
    C2D_DrawRectSolid(POP_X + 8, by, 0.5f, 84, 24, C2D_Color32(150, 50, 50, 255));
    text(POP_X + 8 + 42, by + 5, 0.42f, COL_FG, C2D_AlignCenter, "DELETE");
    C2D_DrawRectSolid(POP_X + POP_W - 8 - 84, by, 0.5f, 84, 24, COL_BTN);
    text(POP_X + POP_W - 8 - 42, by + 5, 0.42f, COL_FG, C2D_AlignCenter, "CLOSE");
}

// ------------------------------------------------------------------ touch ---

static void carousel_touch_down(int x) {
    if (x < 4 + CAR_ARROW_W) { if (car_scroll > 0) car_scroll--; return; }
    if (x >= BOT_W - 4 - CAR_ARROW_W) {
        if (car_scroll + CAR_VISIBLE < NODE_CLASS_COUNT) car_scroll++;
        return;
    }
    int slot = (x - CAR_X0) / (CAR_TILE_W + CAR_GAP);
    int cls = car_scroll + slot;
    if (slot >= 0 && slot < CAR_VISIBLE && cls < NODE_CLASS_COUNT) { drag = DRAG_NEW; drag_class = cls; }
}

static void toolbar_touch_down(int x) {
    for (int b = 0; b < TB_COUNT; b++) {
        if (x < tb_x[b] || x >= tb_x[b] + tb_w[b]) continue;
        tb_pressed = b;
        switch (b) {
        case TB_MODE:  pan_mode = !pan_mode; break;
        case TB_ZOUT:  zoom_by(1 / 1.25f); break;
        case TB_FIT:   zoom_fit(); break;
        case TB_ZIN:   zoom_by(1.25f); break;
        case TB_COPY:  copy_selection(); break;
        case TB_PASTE: paste_clip(); break;
        case TB_DEL:   delete_selection(); break;
        }
    }
}

// Display order for the carousel: OSC, NOISE, ENV
static const NodeType car_order[NODE_CLASS_COUNT] = { NODE_OSC, NODE_NOISE, NODE_ENV };

void synth_page_touch(bool down, bool held, bool up, int x, int y) {
    if (held) { prev_x = cur_x; prev_y = cur_y; cur_x = x; cur_y = y; }

    if (down) {
        down_x = cur_x = prev_x = x; down_y = cur_y = prev_y = y;
        drag = DRAG_NONE; drag_moved = false; tb_pressed = -1;
        if (popup >= 0) { popup_touch_down(x, y); return; }
        if (y >= CAR_Y && y < CAR_Y + CAR_H) { carousel_touch_down(x); return; }
        if (y >= TB_Y && y < TB_Y + TB_H) { toolbar_touch_down(x); return; }
        if (!in_canvas(x, y)) return;

        int i;
        if ((i = out_port_at(x, y)) >= 0) { drag = DRAG_WIRE; drag_node = i; return; }
        if ((i = in_port_at(x, y)) >= 0 && syn_nodes[i].input >= 0) {   // pick up the cable
            drag = DRAG_WIRE; drag_node = syn_nodes[i].input;
            syn_disconnect(i);
            return;
        }
        if ((i = node_at(x, y)) >= 0) {
            drag = DRAG_NODE; drag_node = i;
            if (!selected[i]) { clear_selection(); selected[i] = true; }
            return;
        }
        drag = pan_mode ? DRAG_PAN : DRAG_BOX;
        return;
    }

    if (held) {
        if (abs(cur_x - down_x) > 3 || abs(cur_y - down_y) > 3) drag_moved = true;
        float dx = (cur_x - prev_x) / zoom, dy = (cur_y - prev_y) / zoom;
        if (drag == DRAG_NODE && drag_moved) {
            for (int i = 0; i < SYN_MAX_NODES; i++)      // move the whole selection
                if (syn_nodes[i].used && selected[i]) { syn_nodes[i].x += dx; syn_nodes[i].y += dy; }
        } else if (drag == DRAG_PAN) {
            pan_x -= dx; pan_y -= dy;
        } else if (drag == DRAG_PARAM && popup >= 0) {
            set_param_from_x(popup, drag_param, cur_x);
        } else if (drag == DRAG_WAVE && popup >= 0) {
            wave_draw_at(popup, cur_x, cur_y);
        }
    }

    if (up) {
        if (drag == DRAG_WIRE) {
            int dst = in_port_at(cur_x, cur_y);
            if (dst < 0) dst = node_at(cur_x, cur_y);
            if (dst >= 0) syn_connect(drag_node, dst);
        } else if (drag == DRAG_NODE && !drag_moved) {
            clear_selection();
            selected[drag_node] = true;
            popup = drag_node;
        } else if (drag == DRAG_BOX) {
            clear_selection();
            if (drag_moved) {
                float x0 = fminf(down_x, cur_x), x1 = fmaxf(down_x, cur_x);
                float y0 = fminf(down_y, cur_y), y1 = fmaxf(down_y, cur_y);
                for (int i = 0; i < SYN_MAX_NODES; i++) {
                    const SynNode *n = &syn_nodes[i];
                    if (!n->used) continue;
                    float nx = node_sx(n), ny = node_sy(n);
                    if (nx < x1 && nx + NODE_W * zoom > x0 && ny < y1 && ny + NODE_H * zoom > y0) selected[i] = true;
                }
            }
        } else if (drag == DRAG_NEW) {
            float nx, ny;
            if (in_canvas(cur_x, cur_y)) { nx = wx(cur_x) - NODE_W / 2.0f; ny = wy(cur_y) - NODE_H / 2.0f; }
            else free_spot(&nx, &ny);
            int i = syn_add_node(car_order[drag_class], nx, ny);
            if (i >= 0) { clear_selection(); selected[i] = true; }
        }
        drag = DRAG_NONE;
        tb_pressed = -1;
    }
}

void synth_page_buttons(u32 kDown, u32 kHeld) {
    if (popup >= 0) return;
    if (kDown & KEY_L) zoom_by(1 / 1.25f);
    if (kDown & KEY_R) zoom_by(1.25f);
    float step = 4.0f / zoom;                     // D-pad pans
    if (kHeld & KEY_DLEFT)  pan_x -= step;
    if (kHeld & KEY_DRIGHT) pan_x += step;
    if (kHeld & KEY_DUP)    pan_y -= step;
    if (kHeld & KEY_DDOWN)  pan_y += step;
}

// ------------------------------------------------------------------- draw ---

static void node_subtitle(const SynNode *n, char *out, size_t len) {
    const NodeClass *c = &syn_classes[n->type];
    switch (n->type) {
    case NODE_NOISE: snprintf(out, len, "%s", c->params[NOISE_TYPE].labels[(int)n->p[NOISE_TYPE]]); break;
    case NODE_ENV:   snprintf(out, len, "%d/%d ms", (int)n->p[ENV_A], (int)n->p[ENV_D]); break;
    case NODE_OSC:   snprintf(out, len, "%s %d Hz", c->params[OSC_WAVE].labels[(int)n->p[OSC_WAVE]], (int)lroundf(n->p[OSC_FREQ])); break;
    default:         out[0] = 0;
    }
}

static void draw_canvas(void) {
    C2D_DrawRectSolid(CV_X, CV_Y, 0.5f, CV_W, CV_H, C2D_Color32(12, 15, 19, 255));

    // Grid dots every 20 world units
    float g = 20 * zoom;
    if (g >= 8) {
        float gx0 = sx(floorf(pan_x / 20) * 20), gy0 = sy(floorf(pan_y / 20) * 20);
        for (float gy = gy0; gy < CV_Y + CV_H; gy += g)
            for (float gx = gx0; gx < CV_X + CV_W; gx += g)
                if (gx >= CV_X && gy >= CV_Y) C2D_DrawRectSolid(gx, gy, 0.5f, 1, 1, C2D_Color32(45, 52, 62, 255));
    }

    // Wires
    u32 wire = C2D_Color32(175, 180, 195, 255);
    for (int i = 0; i < SYN_MAX_NODES; i++) {
        const SynNode *n = &syn_nodes[i];
        if (!n->used || n->input < 0) continue;
        const SynNode *s = &syn_nodes[n->input];
        C2D_DrawLine(sx(s->x + NODE_W), port_sy(s), wire, sx(n->x), port_sy(n), wire, 1.5f, 0.5f);
    }
    if (drag == DRAG_WIRE) {
        const SynNode *s = &syn_nodes[drag_node];
        C2D_DrawLine(sx(s->x + NODE_W), port_sy(s), COL_ACCENT, cur_x, cur_y, COL_ACCENT, 1.5f, 0.5f);
    }

    // Nodes
    float w = NODE_W * zoom, h = NODE_H * zoom, pt = PORT * fminf(zoom, 1.3f);
    for (int i = 0; i < SYN_MAX_NODES; i++) {
        const SynNode *n = &syn_nodes[i];
        if (!n->used) continue;
        float x = node_sx(n), y = node_sy(n);
        if (x > CV_X + CV_W || y > CV_Y + CV_H || x + w < CV_X || y + h < CV_Y) continue;
        const NodeClass *c = &syn_classes[n->type];
        bool sel = selected[i] || i == popup;
        C2D_DrawRectSolid(x, y, 0.5f, w, h, sel ? COL_PANEL2 : C2D_Color32(32, 38, 46, 255));
        rect_outline(x, y, w, h, sel ? COL_ACCENT : (n->outputs ? COL_FG : COL_LINE));
        // Output tags: one coloured square per shape, right to left
        int tag = 0;
        for (int t = 4; t >= 0; t--)
            if ((n->outputs >> t) & 1) {
                float ts = 6 * fminf(zoom, 1.3f);
                C2D_DrawRectSolid(x + w - (tag + 1) * (ts + 2) - 1, y + 3, 0.5f, ts, ts, track_color(t, 0, false));
                tag++;
            }
        text(x + 5 * zoom, y + 2 * zoom, 0.42f * zoom, COL_FG, 0, "%s", c->name);
        if (zoom >= 0.6f) {
            char sub[24];
            node_subtitle(n, sub, sizeof sub);
            text(x + 5 * zoom, y + 16 * zoom, 0.32f * zoom, COL_DIM, 0, "%s", sub);
        }
        if (c->has_input)
            C2D_DrawRectSolid(x - pt / 2, port_sy(n) - pt / 2, 0.5f, pt, pt, n->input >= 0 ? wire : COL_LINE);
        C2D_DrawRectSolid(x + w - pt / 2, port_sy(n) - pt / 2, 0.5f, pt, pt, wire);
    }

    // Box-select rectangle
    if (drag == DRAG_BOX && drag_moved) {
        float x0 = fminf(down_x, cur_x), y0 = fminf(down_y, cur_y);
        C2D_DrawRectSolid(x0, y0, 0.5f, fabsf(cur_x - down_x), fabsf(cur_y - down_y), C2D_Color32(52, 195, 160, 40));
        rect_outline(x0, y0, fabsf(cur_x - down_x), fabsf(cur_y - down_y), COL_ACCENT);
    }

    // Mask anything that spilled outside the canvas (zoomed nodes, wires)
    u32 bg = COL_BG_BOT;
    C2D_DrawRectSolid(0, 0, 0.5f, BOT_W, CV_Y, bg);
    C2D_DrawRectSolid(0, CV_Y + CV_H, 0.5f, BOT_W, 240 - CV_Y - CV_H, bg);
    C2D_DrawRectSolid(0, CV_Y, 0.5f, CV_X, CV_H, bg);
    C2D_DrawRectSolid(CV_X + CV_W, CV_Y, 0.5f, BOT_W - CV_X - CV_W, CV_H, bg);
    rect_outline(CV_X, CV_Y, CV_W, CV_H, COL_PANEL2);

    if (!syn_audio_ok())
        text(CV_X + 6, CV_Y + CV_H - 14, 0.34f, C2D_Color32(240, 90, 90, 255), 0,
             "NO AUDIO: needs sdmc:/3ds/dspfirm.cdc");
}

static void draw_carousel(void) {
    C2D_DrawRectSolid(4, CAR_Y, 0.5f, CAR_ARROW_W, CAR_H, car_scroll > 0 ? COL_BTN : COL_PANEL);
    text(4 + CAR_ARROW_W / 2, CAR_Y + 5, 0.5f, car_scroll > 0 ? COL_FG : COL_DIM, C2D_AlignCenter, "<");
    bool more = car_scroll + CAR_VISIBLE < NODE_CLASS_COUNT;
    C2D_DrawRectSolid(BOT_W - 4 - CAR_ARROW_W, CAR_Y, 0.5f, CAR_ARROW_W, CAR_H, more ? COL_BTN : COL_PANEL);
    text(BOT_W - 4 - CAR_ARROW_W / 2, CAR_Y + 5, 0.5f, more ? COL_FG : COL_DIM, C2D_AlignCenter, ">");
    for (int s = 0; s < CAR_VISIBLE; s++) {
        float tx = CAR_X0 + s * (CAR_TILE_W + CAR_GAP);
        int slot = car_scroll + s;
        if (slot >= NODE_CLASS_COUNT) { rect_outline(tx, CAR_Y, CAR_TILE_W, CAR_H, COL_PANEL); continue; }
        bool active = drag == DRAG_NEW && drag_class == slot;
        C2D_DrawRectSolid(tx, CAR_Y, 0.5f, CAR_TILE_W, CAR_H, active ? COL_PANEL2 : COL_PANEL);
        rect_outline(tx, CAR_Y, CAR_TILE_W, CAR_H, active ? COL_ACCENT : COL_LINE);
        text(tx + CAR_TILE_W / 2, CAR_Y + 5, 0.45f, COL_FG, C2D_AlignCenter, "+ %s", syn_classes[car_order[slot]].name);
    }
}

static void draw_toolbar(void) {
    int nsel = selection_count();
    for (int b = 0; b < TB_COUNT; b++) {
        bool enabled = true, lit = tb_pressed == b;
        char label[16];
        switch (b) {
        case TB_MODE:  snprintf(label, sizeof label, pan_mode ? "PAN" : "BOX"); lit = lit || pan_mode; break;
        case TB_ZOUT:  snprintf(label, sizeof label, "-"); break;
        case TB_FIT:   snprintf(label, sizeof label, "%d%%", (int)lroundf(zoom * 100)); break;
        case TB_ZIN:   snprintf(label, sizeof label, "+"); break;
        case TB_COPY:  snprintf(label, sizeof label, "COPY"); enabled = nsel > 0; break;
        case TB_PASTE: snprintf(label, sizeof label, nclip ? "PASTE %d" : "PASTE", nclip); enabled = nclip > 0; break;
        case TB_DEL:   snprintf(label, sizeof label, "DEL"); enabled = nsel > 0; break;
        }
        C2D_DrawRectSolid(tb_x[b], TB_Y, 0.5f, tb_w[b], TB_H, lit ? COL_ACCENT : (enabled ? COL_BTN : COL_PANEL));
        text(tb_x[b] + tb_w[b] / 2, TB_Y + 2, 0.38f, enabled ? COL_FG : COL_DIM, C2D_AlignCenter, "%s", label);
    }
}

void synth_page_draw(void) {
    draw_canvas();                  // first, so its overflow mask sits under the chrome
    draw_carousel();
    draw_toolbar();
}
