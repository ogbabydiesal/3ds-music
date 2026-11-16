// eucseq — stereoscopic polyrhythmic / Euclidean sequencer that sends OSC.
//
// The ring is one bar (4 beats). Five tracks, each a wireframe shape type
// (cube, sphere, pyramid, diamond, cylinder), each subdivide the ring on their
// own: DIVS evenly spaced positions in that track's lane, PULSES of them filled
// using a Euclidean distribution, ROTATE shifts them. A star sweeps the ring;
// when it reaches an unmuted object it sends
//
//     /shape_<track>/pulse <object number 1..DIVS>
//
// Bottom screen: pages selected with < > in the bottom bar.
//   SEQUENCER: pick a track, mute it, change DIVS / PULSES / ROTATE.
//   SYNTH:     patch noise / envelope nodes into instruments played by the shapes.

#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcfg.h"
#include "modseq.h"
#include "page_synth.h"
#include "pattern.h"
#include "settings.h"
#include "store.h"
#include "synth.h"
#include "ui.h"

#define NTRACKS     5
#define MAX_DIVS    32
#define BEATS_PER_CYCLE 4.0

// ------------------------------------------------------------ sequencer ---

typedef struct {
    int  divs, pulses, rot;     // rot: angle offset in degrees, 0..359
    bool mute;
    u8   hit[MAX_DIVS];
    u64  flash_tick[MAX_DIVS];
} Track;

typedef struct {
    int    bpm;
    bool   playing;
    double pos;                 // position in cycles, [0, 1)
    u64    last_tick;
    Track  tr[NTRACKS];
} Seq;

static Seq seq = {
    .bpm = 120,
    .tr = {
        { .divs = 4, .pulses = 4 },     // cube
        { .divs = 3, .pulses = 3 },     // sphere
        { .divs = 8, .pulses = 5 },     // pyramid
        { .divs = 0, .pulses = 0 },     // diamond
        { .divs = 0, .pulses = 0 },     // cylinder
    },
};
static LightLock seq_lock;
static volatile bool seq_thread_run = true;

const char *track_names[NTRACKS] = { "CUBE", "SPHERE", "PYRAMID", "DIAMOND", "CYLINDER" };
const u8 track_rgb[NTRACKS][3] = {
    { 235,  45,  45 },   // red
    { 250, 210,  35 },   // yellow
    {  45, 105, 250 },   // blue
    {  40, 205,  90 },   // green
    { 225,  65, 225 },   // magenta
};

// Euclidean distribution of `pulses` over `divs` positions.
// (Rotation is an angle offset applied at draw/trigger time, not a step shift.)
static void track_rebuild(Track *t) {
    for (int i = 0; i < MAX_DIVS; i++) t->hit[i] = 0;
    if (t->divs <= 0) return;
    for (int i = 0; i < t->divs; i++) {
        t->hit[i] = ((i * t->pulses) % t->divs) < t->pulses;
    }
}

// Caller holds seq_lock.
static void apply_pattern_locked(const Pattern *p) {
    seq.bpm = p->bpm;
    for (int t = 0; t < NTRACKS; t++) {
        seq.tr[t].divs = p->tr[t].divs;
        seq.tr[t].pulses = p->tr[t].pulses;
        seq.tr[t].rot = p->tr[t].rot;
        seq.tr[t].mute = p->tr[t].mute;
        track_rebuild(&seq.tr[t]);
    }
}

void seq_get_pattern(Pattern *out) {
    LightLock_Lock(&seq_lock);
    out->bpm = seq.bpm;
    for (int t = 0; t < NTRACKS; t++)
        out->tr[t] = (PatTrack){ seq.tr[t].divs, seq.tr[t].pulses, seq.tr[t].rot, seq.tr[t].mute };
    LightLock_Unlock(&seq_lock);
}

void seq_set_pattern(const Pattern *p) {
    LightLock_Lock(&seq_lock);
    apply_pattern_locked(p);
    LightLock_Unlock(&seq_lock);
}

bool seq_is_playing(void) { return seq.playing; }

static void seq_set_playing(bool p) {
    LightLock_Lock(&seq_lock);
    if (p && !seq.playing) {
        seq.pos = -1e-9;                    // so position 0 fires immediately
        mod_reset();                        // STEPS lanes restart at step 1
        seq.last_tick = svcGetSystemTick();
        Pattern sp;
        if (song_on_play(&sp)) apply_pattern_locked(&sp);   // SONG mode: start at step 1
    }
    seq.playing = p;
    LightLock_Unlock(&seq_lock);
}

typedef struct { u8 track, obj; } Fire;

// ~1 kHz so pulses aren't quantised to the 60 Hz frame rate.
static void seq_thread(void *arg) {
    (void)arg;
    while (seq_thread_run) {
        Fire fire[NTRACKS * MAX_DIVS];
        int nfire = 0;

        LightLock_Lock(&seq_lock);
        u64 now = svcGetSystemTick();
        if (seq.playing) {
            double dt = (double)(now - seq.last_tick) / SYSCLOCK_ARM11;
            double old = seq.pos;
            seq.pos += dt * seq.bpm / 60.0 / BEATS_PER_CYCLE;
            if (seq.pos >= 1.0) {                           // bar line: SONG may switch pattern
                Pattern sp;
                if (song_on_bar(&sp)) apply_pattern_locked(&sp);
            }
            for (int t = 0; t < NTRACKS; t++) {
                Track *tr = &seq.tr[t];
                if (tr->divs <= 0) continue;
                double off = tr->rot / 360.0;   // object i sits at i/divs + off of the bar
                long a = (long)floor((old - off) * tr->divs), b = (long)floor((seq.pos - off) * tr->divs);
                for (long k = a + 1; k <= b; k++) {
                    int s = (int)(((k % tr->divs) + tr->divs) % tr->divs);
                    if (!tr->hit[s] || tr->mute) continue;
                    tr->flash_tick[s] = now;
                    if (nfire < (int)(sizeof fire / sizeof fire[0]))
                        fire[nfire++] = (Fire){ (u8)(t + 1), (u8)(s + 1) };
                }
            }
            if (seq.pos >= 1.0) seq.pos -= floor(seq.pos);
        }
        seq.last_tick = now;
        LightLock_Unlock(&seq_lock);

        for (int i = 0; i < nfire; i++) {
            char addr[32];
            snprintf(addr, sizeof addr, "/shape_%d/pulse", fire[i].track);
            if (settings_osc_on()) net_send(addr, "i", (int)fire[i].obj);   // OSC off by default
            mod_on_pulse(fire[i].track - 1);  // STEPS lanes set their parameters first...
            syn_trigger(fire[i].track - 1);   // ...then play any instrument assigned to this shape
        }
        svcSleepThread(1000000LL);  // 1 ms
    }
}

// ------------------------------------------------------------- 3D math ---

typedef struct { float x, y, z; } V3;

#define RING_R      100.0f          // the star's circle
#define LANE_R0     86.0f           // outermost lane (track 1)
#define LANE_STEP   11.0f           // lanes step inward
#define OBJ_SIZE    5.0f
#define CAM_D       260.0f
#define EYE_SEP_MAX 28.0f
#define SCREEN_CX   200.0f
#define SCREEN_CY   124.0f

static float view_tilt = 0.38f;
static float view_yaw  = 0.0f;

static V3 rot_x(V3 p, float a) { float c = cosf(a), s = sinf(a); return (V3){ p.x, p.y * c - p.z * s, p.y * s + p.z * c }; }
static V3 rot_y(V3 p, float a) { float c = cosf(a), s = sinf(a); return (V3){ p.x * c + p.z * s, p.y, -p.x * s + p.z * c }; }
static V3 view(V3 p) { return rot_y(rot_x(p, view_tilt), view_yaw); }

// Off-axis stereo: z=0 is the screen plane, negative z pops out.
static void project(V3 p, float eye_x, float *sx, float *sy) {
    float d = CAM_D + p.z;
    if (d < 1.0f) d = 1.0f;
    float k = CAM_D / d;
    *sx = SCREEN_CX + eye_x + (p.x - eye_x) * k;
    *sy = SCREEN_CY - p.y * k;
}

static void line3(V3 a, V3 b, u32 col, float eye_x) {
    float x0, y0, x1, y1;
    project(a, eye_x, &x0, &y0);
    project(b, eye_x, &x1, &y1);
    C2D_DrawLine(x0, y0, col, x1, y1, col, 1.0f, 0.5f);
}

static V3 ring_point(float angle, float r) {   // 0 at 12 o'clock, clockwise
    return (V3){ r * sinf(angle), r * cosf(angle), 0 };
}

// Objects sit on the ring line; EXPAND slides each track into its own lane.
static bool  expand_on  = false;
static float expand_amt = 0.0f;     // animated 0..1 toward expand_on

static float lane_radius(int t) {
    float lane = LANE_R0 - t * LANE_STEP;
    return RING_R + (lane - RING_R) * expand_amt;
}

// ------------------------------------------------------- wireframe shapes ---

typedef struct { V3 v[40]; int nv; u8 e[48][2]; int ne; } Mesh;
static Mesh meshes[NTRACKS];

static int add_v(Mesh *m, float x, float y, float z) { m->v[m->nv] = (V3){ x, y, z }; return m->nv++; }
static void add_e(Mesh *m, int a, int b) { m->e[m->ne][0] = a; m->e[m->ne][1] = b; m->ne++; }

// Circle around `axis` (0=x,1=y,2=z) at offset `off` along that axis.
static int add_circle(Mesh *m, int axis, float r, float off, int seg) {
    int s = m->nv;
    for (int i = 0; i < seg; i++) {
        float a = 2.0f * (float)M_PI * i / seg, c = r * cosf(a), sn = r * sinf(a);
        if (axis == 0) add_v(m, off, c, sn);
        else if (axis == 1) add_v(m, c, off, sn);
        else add_v(m, c, sn, off);
    }
    for (int i = 0; i < seg; i++) add_e(m, s + i, s + (i + 1) % seg);
    return s;
}

static void build_meshes(void) {
    Mesh *m;
    // Cube
    m = &meshes[0];
    for (int i = 0; i < 8; i++) add_v(m, (i & 1) ? 1 : -1, (i & 2) ? 1 : -1, (i & 4) ? 1 : -1);
    for (int i = 0; i < 8; i++)
        for (int b = 1; b < 8; b <<= 1)
            if (!(i & b)) add_e(m, i, i | b);
    // Sphere: three orthogonal great circles
    m = &meshes[1];
    add_circle(m, 0, 1.15f, 0, 12);
    add_circle(m, 1, 1.15f, 0, 12);
    add_circle(m, 2, 1.15f, 0, 12);
    // Pyramid: square base + apex
    m = &meshes[2];
    int b0 = add_v(m, -1, -0.8f, -1); add_v(m, 1, -0.8f, -1); add_v(m, 1, -0.8f, 1); add_v(m, -1, -0.8f, 1);
    int ap = add_v(m, 0, 1.2f, 0);
    for (int i = 0; i < 4; i++) { add_e(m, b0 + i, b0 + (i + 1) % 4); add_e(m, b0 + i, ap); }
    // Diamond: stretched octahedron
    m = &meshes[3];
    add_v(m, 0.9f, 0, 0); add_v(m, -0.9f, 0, 0); add_v(m, 0, 1.4f, 0); add_v(m, 0, -1.4f, 0); add_v(m, 0, 0, 0.9f); add_v(m, 0, 0, -0.9f);
    for (int a = 0; a < 6; a++)
        for (int b = a + 1; b < 6; b++)
            if (a / 2 != b / 2) add_e(m, a, b);
    // Cylinder
    m = &meshes[4];
    int c0 = add_circle(m, 1, 0.95f, -1.0f, 8);
    int c1 = add_circle(m, 1, 0.95f,  1.0f, 8);
    for (int i = 0; i < 8; i += 2) add_e(m, c0 + i, c1 + i);
}

u32 track_color(int t, float flash, bool muted) {
    const u8 *c = track_rgb[t];
    float k = muted ? 0.22f : 1.0f;
    int r = (int)(c[0] * k + (255 - c[0]) * flash);
    int g = (int)(c[1] * k + (255 - c[1]) * flash);
    int b = (int)(c[2] * k + (255 - c[2]) * flash);
    return C2D_Color32(r, g, b, 255);
}

static void draw_mesh(const Mesh *m, V3 center, float size, float spin, float eye_x, u32 col) {
    V3 pts[40];
    for (int i = 0; i < m->nv; i++) {
        V3 p = { m->v[i].x * size, m->v[i].y * size, m->v[i].z * size };
        p = rot_x(rot_y(p, spin), 0.45f);
        pts[i] = view((V3){ center.x + p.x, center.y + p.y, center.z + p.z });
    }
    for (int i = 0; i < m->ne; i++) line3(pts[m->e[i][0]], pts[m->e[i][1]], col, eye_x);
}

// 5-point star as a billboard in view space.
static void draw_star(V3 c, float spin, float eye_x, u32 col) {
    V3 p[10];
    for (int i = 0; i < 10; i++) {
        float r = (i & 1) ? 3.6f : 9.0f;
        float a = spin + i * (float)M_PI / 5.0f;
        p[i] = (V3){ c.x + r * sinf(a), c.y + r * cosf(a), c.z };
    }
    for (int i = 0; i < 10; i++) line3(p[i], p[(i + 1) % 10], col, eye_x);
}

// ------------------------------------------------------------------ text ---

C2D_TextBuf tbuf;
C2D_Font    ui_font = NULL;

void rect_outline(float x, float y, float w, float h, u32 col) {
    C2D_DrawRectSolid(x, y, 0.5f, w, 1, col);
    C2D_DrawRectSolid(x, y + h - 1, 0.5f, w, 1, col);
    C2D_DrawRectSolid(x, y, 0.5f, 1, h, col);
    C2D_DrawRectSolid(x + w - 1, y, 0.5f, 1, h, col);
}

void text(float x, float y, float scale, u32 col, u32 align, const char *fmt, ...) {
    char s[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    C2D_Text t;
    C2D_TextFontParse(&t, ui_font, tbuf, s);    // ui_font NULL -> system font
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | align, x, y, 0.5f, scale, scale, col);
}

// --------------------------------------------------------------- top scene ---

static int sel_track = 0;

static void draw_top(const Seq *s, float eye_x, float t, u64 now) {
    const float TAU = 2.0f * (float)M_PI;

    // The star's ring
    const int SEG = 96;
    u32 ring_col = C2D_Color32(215, 215, 225, 255);
    V3 prev = view(ring_point(0, RING_R));
    for (int i = 1; i <= SEG; i++) {
        V3 cur = view(ring_point(TAU * i / SEG, RING_R));
        line3(prev, cur, ring_col, eye_x);
        prev = cur;
    }

    // Playhead: star on the ring + a faint spoke through the lanes
    double pos = s->pos < 0 ? 0 : s->pos;
    float sa = TAU * (float)pos;
    if (expand_amt > 0.01f)
        line3(view(ring_point(sa, lane_radius(NTRACKS - 1) - 6)), view(ring_point(sa, RING_R)),
              C2D_Color32(70, 75, 90, 255), eye_x);

    // Tracks
    for (int tr = 0; tr < NTRACKS; tr++) {
        const Track *T = &s->tr[tr];
        float r = lane_radius(tr);
        for (int i = 0; i < T->divs; i++) {
            float a = TAU * ((float)i / T->divs + T->rot / 360.0f);
            if (!T->hit[i]) {   // empty subdivision: small dim dot-tick
                line3(view(ring_point(a, r - 1.5f)), view(ring_point(a, r + 1.5f)),
                      track_color(tr, 0, true), eye_x);
                continue;
            }
            float age = T->flash_tick[i] ? (float)(now - T->flash_tick[i]) / SYSCLOCK_ARM11 : 10.0f;
            float flash = age < 0.2f ? 1.0f - age / 0.2f : 0.0f;
            draw_mesh(&meshes[tr], ring_point(a, r), OBJ_SIZE * (1.0f + 0.7f * flash),
                      t * 0.9f + i * 0.6f + tr, eye_x, track_color(tr, flash, T->mute));
        }
    }

    V3 c = view(ring_point(sa, RING_R));
    c.z -= 10.0f;
    draw_star(c, -t * 2.0f, eye_x, C2D_Color32(255, 255, 255, 255));

    // HUD
    u32 dim = C2D_Color32(140, 145, 160, 255);
    const Track *T = &s->tr[sel_track];
    text(8, 6, 0.5f, dim, 0, "%d BPM", s->bpm);
    text(8, 22, 0.5f, track_color(sel_track, 0, T->mute), 0, "%s  E(%d,%d) rot %d\u00B0%s",
         track_names[sel_track], T->pulses, T->divs, T->rot, T->mute ? "  MUTED" : "");
    text(TOP_W - 8, 6, 0.5f, s->playing ? C2D_Color32(52, 195, 160, 255) : dim,
         C2D_AlignRight, s->playing ? "PLAYING" : "STOPPED");
    if (song_mode_on()) {
        int st, n, bar, bars;
        song_status(&st, &n, &bar, &bars);
        text(TOP_W - 8, 22, 0.5f, dim, C2D_AlignRight, "SONG %d/%d  bar %d/%d", st + 1, n, bar, bars);
    }
}

// ------------------------------------------------------------ bottom UI ---

enum { ROW_DIVS, ROW_PULSES, ROW_ROT, ROW_BPM, NROWS };
static const char *row_names[3] = { "DIVS", "PULSES", "ROTATE" };
static int sel_row = ROW_DIVS;

#define TRK_Y     24
#define TRK_W     62
#define TRK_H     52
#define TRK_SEL_H 34                  // top part selects, bottom strip mutes
#define ROW_Y(r)  (82 + (r) * 40)
#define ROW_H     36
#define MINUS_X   116
#define PLUS_X    256
#define BTN_W     54
#define BAR_Y     204
#define BAR_H     32
#define HDR_IP_BTN_X 288          // settings gear button in the header
#define HDR_SAVE_X   96           // SAVE / LOAD / SONG buttons in the header
#define HDR_LOAD_X   158
#define HDR_SONG_X   220
#define HDR_BTN_W    58
#define PAGE_PREV_X  224          // < > page arrows in the bottom bar
#define PAGE_NEXT_X  271
#define PAGE_BTN_W   45

enum { PAGE_SEQ, PAGE_SYNTH, PAGE_MOD, NPAGES };
static const char *page_names[NPAGES] = { "SEQUENCER", "SYNTH", "STEPS" };
static int page = PAGE_SEQ;

// Hit-test ids
enum {
    BTN_NONE = -1,
    // 0..5   : row r minus/plus = r*2 + (0|1)  (rows 0..2)
    BTN_BPM_MINUS = 6, BTN_BPM_PLUS = 7,
    BTN_PLAY = 100, BTN_NET = 101, BTN_EXPAND = 102, BTN_PAGE_PREV = 103, BTN_PAGE_NEXT = 104,
    BTN_SAVE = 105, BTN_LOAD = 106, BTN_SONG = 107,
    BTN_ROW = 200,          // + row
    BTN_TRACK = 300,        // + track
    BTN_MUTE = 310,         // + track
};

static int trk_x(int t) { return 4 + t * (TRK_W + 1); }

static int hit_test(int x, int y) {
    if (y < TRK_Y - 2) {                               // header row
        if (x < 92 && page == PAGE_SEQ) return BTN_EXPAND;
        if (x >= HDR_IP_BTN_X) return BTN_NET;
        if (x >= HDR_SAVE_X && x < HDR_SAVE_X + HDR_BTN_W) return BTN_SAVE;
        if (x >= HDR_LOAD_X && x < HDR_LOAD_X + HDR_BTN_W) return BTN_LOAD;
        if (x >= HDR_SONG_X && x < HDR_SONG_X + HDR_BTN_W + 4) return BTN_SONG;
        return BTN_NONE;
    }
    if (page == PAGE_SEQ && y >= TRK_Y && y < TRK_Y + TRK_H) {
        int t = (x - 4) / (TRK_W + 1);
        if (t < 0 || t >= NTRACKS) return BTN_NONE;
        return (y < TRK_Y + TRK_SEL_H ? BTN_TRACK : BTN_MUTE) + t;
    }
    for (int r = 0; r < 3 && page == PAGE_SEQ; r++) {
        if (y < ROW_Y(r) || y >= ROW_Y(r) + ROW_H) continue;
        if (x >= MINUS_X && x < MINUS_X + BTN_W) return r * 2;
        if (x >= PLUS_X && x < PLUS_X + BTN_W) return r * 2 + 1;
        return BTN_ROW + r;
    }
    if (y >= BAR_Y && y < BAR_Y + BAR_H) {
        if (x < 80) return BTN_PLAY;
        if (x < 120) return BTN_BPM_MINUS;
        if (x < 184) return BTN_ROW + ROW_BPM;
        if (x < 222) return BTN_BPM_PLUS;
        if (x >= PAGE_PREV_X && x < PAGE_PREV_X + PAGE_BTN_W) return BTN_PAGE_PREV;
        if (x >= PAGE_NEXT_X && x < PAGE_NEXT_X + PAGE_BTN_W) return BTN_PAGE_NEXT;
        return BTN_NONE;
    }
    return BTN_NONE;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void change_param(int row, int delta) {
    LightLock_Lock(&seq_lock);
    Track *T = &seq.tr[sel_track];
    switch (row) {
    case ROW_DIVS: {
        int old = T->divs;
        T->divs = clampi(T->divs + delta, 0, MAX_DIVS);
        // A "full" track stays full, so adding divisions adds objects.
        T->pulses = (T->pulses == old) ? T->divs : clampi(T->pulses, 0, T->divs);
        break;
    }
    case ROW_PULSES: T->pulses = clampi(T->pulses + delta, 0, T->divs); break;
    case ROW_ROT:    T->rot = ((T->rot + delta) % 360 + 360) % 360; break;
    case ROW_BPM:    seq.bpm = clampi(seq.bpm + delta, 20, 300); break;
    }
    track_rebuild(T);
    LightLock_Unlock(&seq_lock);
}

static void toggle_mute(int t) {
    LightLock_Lock(&seq_lock);
    seq.tr[t].mute = !seq.tr[t].mute;
    LightLock_Unlock(&seq_lock);
}

static void apply_button(int b, int delta_mag) {
    if (b >= 0 && b < 6) { sel_row = b / 2; change_param(b / 2, (b & 1) ? delta_mag : -delta_mag); }
    else if (b == BTN_BPM_MINUS) { sel_row = ROW_BPM; change_param(ROW_BPM, -delta_mag); }
    else if (b == BTN_BPM_PLUS)  { sel_row = ROW_BPM; change_param(ROW_BPM, delta_mag); }
}

static void draw_button(float x, float y, float w, float h, u32 fill, u32 fg, const char *label, float sc) {
    C2D_DrawRectSolid(x, y, 0.5f, w, h, fill);
    text(x + w / 2, y + (h - 30 * sc) / 2, sc, fg, C2D_AlignCenter, "%s", label);
}

static void draw_seq_page(const Seq *s, int pressed);

static void draw_bottom(const Seq *s, int pressed) {
    u32 panel = C2D_Color32(30, 37, 44, 255);
    u32 panel_sel = C2D_Color32(44, 56, 66, 255);
    u32 btn = C2D_Color32(58, 69, 80, 255);
    u32 accent = C2D_Color32(52, 195, 160, 255);
    u32 fg = C2D_Color32(235, 240, 245, 255);
    u32 dim = C2D_Color32(140, 145, 160, 255);

    if (page == PAGE_SYNTH) synth_page_draw();          // canvas first; header/footer draw over it

    // Header left: EXPAND toggle on the sequencer page, page name elsewhere
    if (page == PAGE_SEQ) {
        C2D_DrawRectSolid(4, 2, 0.5f, 86, 19, expand_on ? accent : btn);
        text(47, 4, 0.42f, fg, C2D_AlignCenter, expand_on ? "EXPANDED" : "EXPAND");
    } else {
        text(6, 4, 0.48f, fg, 0, "%s", page_names[page]);
    }
    // SAVE / LOAD / SONG (header, middle). SONG is lit while song mode is on.
    draw_button(HDR_SAVE_X, 2, HDR_BTN_W, 19, pressed == BTN_SAVE ? accent : btn, fg, "SAVE", 0.42f);
    draw_button(HDR_LOAD_X, 2, HDR_BTN_W, 19, pressed == BTN_LOAD ? accent : btn, fg, "LOAD", 0.42f);
    draw_button(HDR_SONG_X, 2, HDR_BTN_W + 4, 19, song_mode_on() || pressed == BTN_SONG ? accent : btn, fg, "SONG", 0.42f);
    // Settings gear (header, right). Green while OSC output is on.
    C2D_DrawRectSolid(HDR_IP_BTN_X, 2, 0.5f, BOT_W - 4 - HDR_IP_BTN_X, 19, pressed == BTN_NET ? accent : btn);
    draw_gear((HDR_IP_BTN_X + BOT_W - 4) / 2.0f, 11.5f, settings_osc_on() ? accent : fg, pressed == BTN_NET ? accent : btn);

    if (page == PAGE_SEQ) draw_seq_page(s, pressed);
    else if (page == PAGE_MOD) mod_page_draw();

    // Bottom bar: play | bpm - value + | < page >
    draw_button(4, BAR_Y, 74, BAR_H, s->playing ? C2D_Color32(190, 60, 60, 255) : accent, fg,
                s->playing ? "STOP" : "PLAY", 0.55f);
    C2D_DrawRectSolid(82, BAR_Y, 0.5f, 138, BAR_H, sel_row == ROW_BPM ? panel_sel : panel);
    draw_button(84, BAR_Y + 3, 34, BAR_H - 6, pressed == BTN_BPM_MINUS ? accent : btn, fg, "-", 0.7f);
    draw_button(184, BAR_Y + 3, 34, BAR_H - 6, pressed == BTN_BPM_PLUS ? accent : btn, fg, "+", 0.7f);
    text(151, BAR_Y + 3, 0.6f, fg, C2D_AlignCenter, "%d", s->bpm);
    text(151, BAR_Y + 19, 0.34f, dim, C2D_AlignCenter, "BPM");
    draw_button(PAGE_PREV_X, BAR_Y, PAGE_BTN_W, BAR_H, pressed == BTN_PAGE_PREV ? accent : btn, fg, "<", 0.6f);
    draw_button(PAGE_NEXT_X, BAR_Y, PAGE_BTN_W, BAR_H, pressed == BTN_PAGE_NEXT ? accent : btn, fg, ">", 0.6f);
    for (int i = 0; i < NPAGES; i++)                       // page dots
        C2D_DrawRectSolid(268 - NPAGES * 3 + i * 6 + 2, BAR_Y + BAR_H - 5, 0.5f, 3, 3, i == page ? fg : COL_LINE);

    if (page == PAGE_SYNTH) synth_page_draw_popup();
    settings_draw();
    store_ui_draw();
}

static void draw_seq_page(const Seq *s, int pressed) {
    u32 panel = COL_PANEL, panel_sel = COL_PANEL2, btn = COL_BTN, accent = COL_ACCENT, fg = COL_FG, dim = COL_DIM;

    // Track selector: top = select, bottom strip = mute
    for (int t = 0; t < NTRACKS; t++) {
        const Track *T = &s->tr[t];
        float x = trk_x(t);
        bool sel = t == sel_track;
        u32 col = track_color(t, 0, T->mute);
        C2D_DrawRectSolid(x, TRK_Y, 0.5f, TRK_W, TRK_H, sel ? panel_sel : panel);
        if (sel) {
            C2D_DrawRectSolid(x, TRK_Y, 0.5f, TRK_W, 2, col);
            C2D_DrawRectSolid(x, TRK_Y + TRK_H - 2, 0.5f, TRK_W, 2, col);
            C2D_DrawRectSolid(x, TRK_Y, 0.5f, 2, TRK_H, col);
            C2D_DrawRectSolid(x + TRK_W - 2, TRK_Y, 0.5f, 2, TRK_H, col);
        }
        text(x + TRK_W / 2, TRK_Y + 3, 0.4f, col, C2D_AlignCenter, "%s", track_names[t]);
        text(x + TRK_W / 2, TRK_Y + 16, 0.45f, T->mute ? dim : fg, C2D_AlignCenter, "%d/%d", T->pulses, T->divs);
        u32 mfill = T->mute ? C2D_Color32(170, 55, 55, 255) : (pressed == BTN_MUTE + t ? btn : C2D_Color32(40, 48, 56, 255));
        C2D_DrawRectSolid(x + 3, TRK_Y + TRK_SEL_H, 0.5f, TRK_W - 6, TRK_H - TRK_SEL_H - 3, mfill);
        text(x + TRK_W / 2, TRK_Y + TRK_SEL_H + 2, 0.38f, T->mute ? fg : dim, C2D_AlignCenter, T->mute ? "MUTED" : "mute");
    }

    // Parameter rows for the selected track
    const Track *T = &s->tr[sel_track];
    int vals[3] = { T->divs, T->pulses, T->rot };
    for (int r = 0; r < 3; r++) {
        float y = ROW_Y(r);
        C2D_DrawRectSolid(4, y, 0.5f, BOT_W - 8, ROW_H, r == sel_row ? panel_sel : panel);
        if (r == sel_row) C2D_DrawRectSolid(4, y, 0.5f, 3, ROW_H, track_color(sel_track, 0, false));
        text(14, y + (ROW_H - 18) / 2, 0.58f, r == sel_row ? fg : dim, 0, "%s", row_names[r]);
        draw_button(MINUS_X, y + 3, BTN_W, ROW_H - 6, pressed == r * 2 ? accent : btn, fg, "-", 0.8f);
        draw_button(PLUS_X, y + 3, BTN_W, ROW_H - 6, pressed == r * 2 + 1 ? accent : btn, fg, "+", 0.8f);
        text((MINUS_X + BTN_W + PLUS_X) / 2.0f, y + (ROW_H - 24) / 2, 0.8f, fg, C2D_AlignCenter, r == ROW_ROT ? "%d\u00B0" : "%d", vals[r]);
    }
}

// ----------------------------------------------------------------- main ---

int main(void) {
    gfxInitDefault();
    gfxSet3D(true);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 4);
    C2D_Prepare();

    C3D_RenderTarget *top_l = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *top_r = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    C3D_RenderTarget *bot   = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    tbuf = C2D_TextBufNew(4096);
    romfsInit();
    ui_font = C2D_FontLoad("romfs:/arial.bcfnt");     // falls back to system font if missing

    build_meshes();
    net_config_load();
    bool net_ok = net_init();
    settings_load();                                  // OSC output + gyro/accel toggles
    store_init();                                     // save slots + song

    syn_init();
    mod_init();
    syn_audio_start();                                // needs sdmc:/3ds/dspfirm.cdc
    LightLock_Init(&seq_lock);
    for (int t = 0; t < NTRACKS; t++) track_rebuild(&seq.tr[t]);

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread th = threadCreate(seq_thread, NULL, 32 * 1024, prio - 1, -1, false);

    hidSetRepeatParameters(20, 4);
    int touch_btn = BTN_NONE, touch_hold = 0;
    touchPosition last_tp = { 0, 0 };
    bool touch_owner_synth = false, touch_owner_mod = false;
    u64 t0 = svcGetSystemTick();

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown(), kHeld = hidKeysHeld(), kRep = hidKeysDownRepeat();

        if (kDown & KEY_START) break;
        if (kDown & KEY_SELECT) { if (settings_is_open()) settings_close(); else settings_open(); }
        settings_tick();
        store_ui_tick();
        if (kDown & KEY_A) seq_set_playing(!seq.playing);
        if (kDown & KEY_B) { LightLock_Lock(&seq_lock); seq.pos = -1e-9; LightLock_Unlock(&seq_lock); }
        if (page == PAGE_SYNTH) synth_page_buttons(kDown, kHeld);     // L/R zoom, D-pad pan
        if (page == PAGE_SEQ) {
            if (kDown & KEY_X) toggle_mute(sel_track);
            if (kDown & KEY_L) sel_track = (sel_track + NTRACKS - 1) % NTRACKS;
            if (kDown & KEY_R) sel_track = (sel_track + 1) % NTRACKS;
            if (kRep & KEY_DUP)    sel_row = (sel_row + NROWS - 1) % NROWS;
            if (kRep & KEY_DDOWN)  sel_row = (sel_row + 1) % NROWS;
            if (kRep & KEY_DLEFT)  change_param(sel_row, (kHeld & KEY_ZR) ? -10 : -1);
            if (kRep & KEY_DRIGHT) change_param(sel_row, (kHeld & KEY_ZR) ? 10 : 1);
        }
        if (kDown & KEY_Y) { view_tilt = 0.38f; view_yaw = 0; }
        if (kDown & KEY_ZL) expand_on = !expand_on;
        expand_amt += ((expand_on ? 1.0f : 0.0f) - expand_amt) * 0.18f;   // ease toward target

        circlePosition cp;
        hidCircleRead(&cp);
        if (abs(cp.dx) > 15) view_yaw  += cp.dx / 156.0f * 0.035f;
        if (abs(cp.dy) > 15) view_tilt -= cp.dy / 156.0f * 0.035f;
        if (view_tilt > 1.4f) view_tilt = 1.4f;
        if (view_tilt < -1.4f) view_tilt = -1.4f;

        // Touch routing: the SYNTH page owns touches that start in its area
        // (or anywhere while its popup is open); everything else is ours.
        bool tdown = kDown & KEY_TOUCH, theld = kHeld & KEY_TOUCH, tup = hidKeysUp() & KEY_TOUCH;
        if (theld) hidTouchRead(&last_tp);
        if (store_ui_is_open()) {                      // SAVE / LOAD / SONG popup owns every touch
            if (tdown) store_ui_touch_down(last_tp.px, last_tp.py);
            touch_owner_synth = false;
            touch_btn = BTN_NONE;
        } else if (settings_is_open()) {               // settings popup owns every touch
            if (tdown) settings_touch_down(last_tp.px, last_tp.py);
            touch_owner_synth = false;
            touch_btn = BTN_NONE;
        } else {
        if (tdown) {
            touch_owner_synth = page == PAGE_SYNTH &&
                (synth_page_popup_open() || (last_tp.py >= SYNTH_AREA_Y0 && last_tp.py < SYNTH_AREA_Y1));
            touch_owner_mod = page == PAGE_MOD && last_tp.py >= MOD_AREA_Y0 && last_tp.py < MOD_AREA_Y1;
        }
        if (touch_owner_mod) {
            if (theld || tup) mod_page_touch(tdown, theld, tup, last_tp.px, last_tp.py);
            touch_btn = BTN_NONE;
        } else if (touch_owner_synth) {
            if (theld || tup) synth_page_touch(tdown, theld, tup, last_tp.px, last_tp.py);
            touch_btn = BTN_NONE;
        } else
        // Touch: tap once, hold to auto-repeat on +/- buttons
        if (kHeld & KEY_TOUCH) {
            touchPosition tp = last_tp;
            int b = hit_test(tp.px, tp.py);
            if (kDown & KEY_TOUCH) {
                touch_btn = b; touch_hold = 0;
                if (b >= BTN_MUTE && b < BTN_MUTE + NTRACKS) { sel_track = b - BTN_MUTE; toggle_mute(sel_track); }
                else if (b >= BTN_TRACK && b < BTN_TRACK + NTRACKS) sel_track = b - BTN_TRACK;
                else if (b >= BTN_ROW && b < BTN_ROW + NROWS) sel_row = b - BTN_ROW;
                else if (b == BTN_PLAY) seq_set_playing(!seq.playing);
                else if (b == BTN_EXPAND) expand_on = !expand_on;
                else if (b == BTN_SAVE) { store_ui_open(STORE_UI_SAVE); touch_btn = BTN_NONE; }
                else if (b == BTN_LOAD) { store_ui_open(STORE_UI_LOAD); touch_btn = BTN_NONE; }
                else if (b == BTN_SONG) { store_ui_open(STORE_UI_SONG); touch_btn = BTN_NONE; }
                else if (b == BTN_NET) { settings_open(); touch_btn = BTN_NONE; }
                else if (b == BTN_PAGE_PREV) page = (page + NPAGES - 1) % NPAGES;
                else if (b == BTN_PAGE_NEXT) page = (page + 1) % NPAGES;
                else apply_button(b, 1);
            } else if (b == touch_btn && b >= 0 && b <= BTN_BPM_PLUS) {
                if (++touch_hold > 24 && touch_hold % 3 == 0)
                    apply_button(b, ((b >= BTN_BPM_MINUS || b / 2 == ROW_ROT) && touch_hold > 90) ? 5 : 1);
            } else {
                touch_btn = BTN_NONE;
            }
        } else {
            touch_btn = BTN_NONE;
        }
        }   // settings closed

        LightLock_Lock(&seq_lock);
        Seq snap = seq;
        LightLock_Unlock(&seq_lock);

        u64 now = svcGetSystemTick();
        float t = (float)(now - t0) / SYSCLOCK_ARM11;
        float sep = osGet3DSliderState() * EYE_SEP_MAX;
        u32 bg = C2D_Color32(8, 10, 14, 255);

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TextBufClear(tbuf);

        C2D_TargetClear(top_l, bg);
        C2D_SceneBegin(top_l);
        draw_top(&snap, -sep / 2, t, now);
        if (!net_ok) text(200, 220, 0.5f, C2D_Color32(240, 80, 80, 255), C2D_AlignCenter, "Network init failed");

        C2D_TargetClear(top_r, bg);
        C2D_SceneBegin(top_r);
        draw_top(&snap, sep / 2, t, now);
        if (!net_ok) text(200, 220, 0.5f, C2D_Color32(240, 80, 80, 255), C2D_AlignCenter, "Network init failed");

        C2D_TargetClear(bot, C2D_Color32(16, 20, 24, 255));
        C2D_SceneBegin(bot);
        draw_bottom(&snap, touch_btn);

        C3D_FrameEnd(0);
    }

    seq_thread_run = false;
    threadJoin(th, U64_MAX);
    threadFree(th);
    syn_audio_stop();
    settings_exit();
    if (ui_font) C2D_FontFree(ui_font);
    romfsExit();
    C2D_TextBufDelete(tbuf);
    C2D_Fini();
    C3D_Fini();
    net_exit();
    gfxExit();
    return 0;
}
