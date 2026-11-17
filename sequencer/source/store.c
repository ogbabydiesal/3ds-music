#include "store.h"
#include "samples.h"
#include "modseq.h"
#include "synth.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __3DS__
static LightLock song_lock;
#define SONG_LOCK()   LightLock_Lock(&song_lock)
#define SONG_UNLOCK() LightLock_Unlock(&song_lock)
#else
#define SONG_LOCK()   ((void)0)
#define SONG_UNLOCK() ((void)0)
#endif

#define APP_DIR       "sdmc:/3ds/nodeTrix"
#ifndef OLD_STORE_DIR
#define OLD_STORE_DIR "sdmc:/3ds/osc3ds/eucseq"     // where slots lived before the rename
#endif
#ifndef STORE_DIR
#define STORE_DIR APP_DIR "/slots"
#endif

// ------------------------------------------------------------- slot files ---

static Pattern cache[STORE_NSLOTS];     // pattern part of each slot (song uses these)
static bool    cache_ok[STORE_NSLOTS];

static void slot_path(int slot, char *out, size_t n) { snprintf(out, n, STORE_DIR "/slot%d.txt", slot + 1); }

static void make_dirs(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(APP_DIR, 0777);
    mkdir(STORE_DIR, 0777);
}

// After the eucseq -> nodeTrix rename: copy any old slot / song file that the
// new folder doesn't have yet. Copies (rather than moving the folder) so it
// works even if the new folder already exists; the old files are left alone.
static bool file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return;
    FILE *out = fopen(to, "wb");
    if (out) {
        char buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
        fclose(out);
    }
    fclose(in);
}

static void migrate_old_slots(void) {
    char from[256], to[256];
    for (int i = 0; i <= STORE_NSLOTS; i++) {          // slot1..slot8, then song.txt
        char name[32];
        if (i < STORE_NSLOTS) snprintf(name, sizeof name, "slot%d.txt", i + 1);
        else snprintf(name, sizeof name, "song.txt");
        snprintf(from, sizeof from, "%s/%s", OLD_STORE_DIR, name);
        snprintf(to, sizeof to, "%s/%s", STORE_DIR, name);
        if (file_exists(from) && !file_exists(to)) copy_file(from, to);
    }
}

static bool write_slot(int slot, const Pattern *p, const SynNode *nodes) {
    make_dirs();
    char path[256];
    slot_path(slot, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "eucseq-slot 2\nbpm %d\n", p->bpm);      // v2: 10 params per node (v1 had 8)
    for (int t = 0; t < PAT_NTRACKS; t++)
        fprintf(f, "track %d %d %d %d %d\n", t, p->tr[t].divs, p->tr[t].pulses, p->tr[t].rot, p->tr[t].mute);
    for (int i = 0; i < SYN_MAX_NODES; i++) {
        const SynNode *n = &nodes[i];
        if (!n->used) continue;
        fprintf(f, "node %d %d %.1f %.1f %d %d", i, n->type, n->x, n->y, n->input, n->outputs);
        for (int k = 0; k < SYN_MAX_PARAMS; k++) fprintf(f, " %g", n->p[k]);
        for (int k = 0; k < SYN_WT_SIZE; k++) fprintf(f, " %g", n->wt[k]);
        for (int k = 0; k < SYN_MAX_PARAMS; k++) fprintf(f, " %d", n->mod_src[k]);   // S1..S8 per param
        fputc('\n', f);
        if (n->type == NODE_SAMPLE && samples_count()) {              // the file by name: indexes shift
            const Sample *sm = samples_get(((int)n->p[SMP_FILE]) % samples_count());
            if (sm) fprintf(f, "sample %d %s\n", i, sm->name);
        }
    }
    for (int i = 0; i < MOD_SEQS; i++) {
        const ModSeq *m = &mod_seqs[i];
        fprintf(f, "seq %d %d %d %g %g", i, m->trig, m->nsteps, m->min, m->max);
        for (int k = 0; k < MOD_MAX_STEPS; k++) fprintf(f, " %g", m->v[k]);
        fputc('\n', f);
    }
    fclose(f);
    return true;
}

// nodes / lanes may be NULL (pattern only)
static bool read_slot(int slot, Pattern *p, SynNode *nodes, ModSeq *seqs) {
    char path[256];
    slot_path(slot, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    memset(p, 0, sizeof *p);
    p->bpm = 120;
    if (nodes) memset(nodes, 0, sizeof(SynNode) * SYN_MAX_NODES);
    if (seqs) mod_defaults(seqs);                      // older saves have no step sequencers
    char line[1024];
    bool ok = false;
    int np = 8;                                         // params per node line: 8 in v1 files
    while (fgets(line, sizeof line, f)) {
        int a, b, c, d, e;
        if (!strncmp(line, "eucseq-slot", 11)) {
            ok = true;
            if (sscanf(line, "eucseq-slot %d", &a) == 1 && a >= 2) np = SYN_MAX_PARAMS;
        }
        else if (sscanf(line, "bpm %d", &a) == 1) p->bpm = a < 20 ? 20 : a > 300 ? 300 : a;
        else if (sscanf(line, "track %d %d %d %d %d", &a, &b, &c, &d, &e) == 5 && a >= 0 && a < PAT_NTRACKS) {
            p->tr[a].divs = b < 0 ? 0 : b > 32 ? 32 : b;
            p->tr[a].pulses = c < 0 ? 0 : c > p->tr[a].divs ? p->tr[a].divs : c;
            p->tr[a].rot = ((d % 360) + 360) % 360;
            p->tr[a].mute = e != 0;
        } else if (seqs && !strncmp(line, "seq ", 4)) {
            char *tok = strtok(line + 4, " \n");
            float v[5 + MOD_MAX_STEPS];
            int nv = 0;
            while (tok && nv < (int)(sizeof v / sizeof v[0])) { v[nv++] = strtof(tok, NULL); tok = strtok(NULL, " \n"); }
            int i = nv ? (int)v[0] : -1;
            if (nv < 5 || i < 0 || i >= MOD_SEQS) continue;
            ModSeq *m = &seqs[i];
            m->trig = (int)v[1]; m->nsteps = (int)v[2]; m->min = v[3]; m->max = v[4];
            for (int k = 0; k < MOD_MAX_STEPS && 5 + k < nv; k++) m->v[k] = v[5 + k];
        } else if (nodes && !strncmp(line, "sample ", 7)) {           // after its node line
            char name[SAMPLES_NAME_LEN];
            if (sscanf(line, "sample %d %39s", &a, name) == 2 && a >= 0 && a < SYN_MAX_NODES && nodes[a].used) {
                int idx = samples_find(name);
                if (idx >= 0) nodes[a].p[SMP_FILE] = (float)idx;
            }
        } else if (nodes && !strncmp(line, "node ", 5)) {
            char *tok = strtok(line + 5, " \n");
            float v[6 + SYN_MAX_PARAMS + SYN_WT_SIZE + SYN_MAX_PARAMS];
            int nv = 0;
            while (tok && nv < (int)(sizeof v / sizeof v[0])) { v[nv++] = strtof(tok, NULL); tok = strtok(NULL, " \n"); }
            if (nv < 6 + np) continue;
            int i = (int)v[0];
            if (i < 0 || i >= SYN_MAX_NODES) continue;
            SynNode *n = &nodes[i];
            n->used = true;
            n->type = (NodeType)(int)v[1];
            n->x = v[2]; n->y = v[3];
            n->input = (int)v[4];
            n->outputs = (uint8_t)(int)v[5];
            const NodeClass *cls = n->type >= 0 && n->type < NODE_CLASS_COUNT ? &syn_classes[n->type] : NULL;
            for (int k = 0; k < SYN_MAX_PARAMS; k++)
                n->p[k] = k < np ? v[6 + k] : (cls && k < cls->nparams ? cls->params[k].def : 0);
            for (int k = 0; k < SYN_WT_SIZE && 6 + np + k < nv; k++) n->wt[k] = v[6 + np + k];
            int ms = 6 + np + SYN_WT_SIZE;                  // S1..S8 assignments (absent in older saves)
            for (int k = 0; k < SYN_MAX_PARAMS; k++) n->mod_src[k] = k < np && ms + k < nv ? (int8_t)(int)v[ms + k] : -1;
        }
    }
    fclose(f);
    return ok;
}

bool store_slot_used(int slot) { return slot >= 0 && slot < STORE_NSLOTS && cache_ok[slot]; }

bool store_save_slot(int slot) {
    Pattern p;
    seq_get_pattern(&p);
    if (!write_slot(slot, &p, syn_nodes)) return false;
    SONG_LOCK();
    cache[slot] = p;
    cache_ok[slot] = true;
    SONG_UNLOCK();
    return true;
}

bool store_load_slot(int slot) {
    static SynNode nodes[SYN_MAX_NODES];
    static ModSeq seqs[MOD_SEQS];
    Pattern p;
    if (!read_slot(slot, &p, nodes, seqs)) return false;
    seq_set_pattern(&p);
    syn_replace_all(nodes);
    mod_set_all(seqs);
    return true;
}

// ------------------------------------------------------------------- song ---

typedef struct { int slot, bars; } SongStep;

static SongStep steps[SONG_MAX] = { { 0, 4 } };
static int  nsteps = 1;
static bool song_on = false;
static volatile int cur = 0, bars_done = 0;

static void song_save(void) {
    make_dirs();
    FILE *f = fopen(STORE_DIR "/song.txt", "w");
    if (!f) return;
    fprintf(f, "eucseq-song 1\non %d\n", song_on);
    for (int i = 0; i < nsteps; i++) fprintf(f, "step %d %d\n", steps[i].slot + 1, steps[i].bars);
    fclose(f);
}

static void song_load(void) {
    FILE *f = fopen(STORE_DIR "/song.txt", "r");
    if (!f) return;
    char line[64];
    int a, b, n = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "on %d", &a) == 1) song_on = a != 0;
        else if (sscanf(line, "step %d %d", &a, &b) == 2 && n < SONG_MAX) {
            steps[n].slot = (a - 1 + STORE_NSLOTS) % STORE_NSLOTS;
            steps[n].bars = b < 1 ? 1 : b > 64 ? 64 : b;
            n++;
        }
    }
    fclose(f);
    if (n) nsteps = n;
}

bool song_mode_on(void) { return song_on; }

bool song_on_play(Pattern *out) {
    bool ok = false;
    SONG_LOCK();
    cur = 0; bars_done = 0;
    if (song_on && nsteps > 0 && cache_ok[steps[0].slot]) { *out = cache[steps[0].slot]; ok = true; }
    SONG_UNLOCK();
    return ok;
}

bool song_on_bar(Pattern *out) {
    bool ok = false;
    SONG_LOCK();
    if (song_on && nsteps > 0) {
        if (cur >= nsteps) cur = 0;
        if (++bars_done >= steps[cur].bars) {
            bars_done = 0;
            cur = (cur + 1) % nsteps;
            if (cache_ok[steps[cur].slot]) { *out = cache[steps[cur].slot]; ok = true; }
        }
    }
    SONG_UNLOCK();
    return ok;
}

void song_status(int *step, int *n, int *bar, int *bars) {
    SONG_LOCK();
    int c = cur < nsteps ? cur : 0;
    *step = c; *n = nsteps; *bar = bars_done + 1; *bars = steps[c].bars;
    SONG_UNLOCK();
}

void store_init(void) {
#ifdef __3DS__
    LightLock_Init(&song_lock);
#endif
    make_dirs();
    migrate_old_slots();
    for (int s = 0; s < STORE_NSLOTS; s++) cache_ok[s] = read_slot(s, &cache[s], NULL, NULL);
    song_load();
}

// --------------------------------------------------------------------- UI ---

static int  ui = -1;                    // which popup, -1 closed
static int  sel_step = 0;
static int  last_slot = -1;
static char msg[48] = "";
static int  msg_frames = 0;

bool store_ui_is_open(void) { return ui >= 0; }
void store_ui_open(int which) { ui = which; msg[0] = 0; if (sel_step >= nsteps) sel_step = nsteps - 1; }
void store_ui_close(void) { ui = -1; }
void store_ui_tick(void) { if (msg_frames > 0 && --msg_frames == 0) msg[0] = 0; }

static void flash(const char *m) { snprintf(msg, sizeof msg, "%s", m); msg_frames = 120; }

static bool in_rect(int x, int y, float rx, float ry, float rw, float rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void panel(float x, float y, float w, float h, const char *title) {
    C2D_DrawRectSolid(0, 0, 0.5f, BOT_W, 240, C2D_Color32(0, 0, 0, 150));
    C2D_DrawRectSolid(x, y, 0.5f, w, h, COL_PANEL);
    rect_outline(x, y, w, h, COL_LINE);
    text(x + 10, y + 5, 0.5f, COL_FG, 0, "%s", title);
}

static void button(float x, float y, float w, float h, u32 fill, const char *label) {
    C2D_DrawRectSolid(x, y, 0.5f, w, h, fill);
    text(x + w / 2, y + (h - 30 * 0.42f) / 2, 0.42f, COL_FG, C2D_AlignCenter, "%s", label);
}

// ---- SAVE / LOAD: 4 x 2 grid of slots ----

#define SL_X   16
#define SL_W   288
#define SL_H   172
#define SL_Y   ((240 - SL_H) / 2)
#define TILE_W 64
#define TILE_H 46
static float tile_x(int s) { return SL_X + 10 + (s % 4) * 68; }
static float tile_y(int s) { return SL_Y + 28 + (s / 4) * 52; }
#define SL_BTN_Y (SL_Y + SL_H - 32)

static void draw_slots(bool saving) {
    panel(SL_X, SL_Y, SL_W, SL_H, saving ? "SAVE" : "LOAD");
    text(SL_X + SL_W - 10, SL_Y + 8, 0.34f, COL_DIM, C2D_AlignRight,
         saving ? "tap a slot to save into it" : "tap a slot to load it");
    for (int s = 0; s < STORE_NSLOTS; s++) {
        float x = tile_x(s), y = tile_y(s);
        bool used = cache_ok[s];
        bool hot = s == last_slot;
        C2D_DrawRectSolid(x, y, 0.5f, TILE_W, TILE_H, used ? COL_PANEL2 : C2D_Color32(24, 29, 35, 255));
        rect_outline(x, y, TILE_W, TILE_H, hot ? COL_ACCENT : COL_LINE);
        text(x + 6, y + 2, 0.5f, used || saving ? COL_FG : COL_DIM, 0, "%d", s + 1);
        if (!used) { text(x + 6, y + 26, 0.32f, COL_DIM, 0, "empty"); continue; }
        text(x + TILE_W - 5, y + 6, 0.32f, COL_DIM, C2D_AlignRight, "%d BPM", cache[s].bpm);
        for (int t = 0; t < PAT_NTRACKS; t++) {                 // one square per active track
            const PatTrack *T = &cache[s].tr[t];
            if (T->divs > 0 && T->pulses > 0)
                C2D_DrawRectSolid(x + 6 + t * 11, y + 30, 0.5f, 8, 8, track_color(t, 0, T->mute));
        }
    }
    if (msg[0]) text(SL_X + 10, SL_BTN_Y + 6, 0.38f, COL_ACCENT, 0, "%s", msg);
    else text(SL_X + 10, SL_BTN_Y + 6, 0.32f, COL_DIM, 0, "slots hold the pattern, synth patch + S1-S8");
    button(SL_X + SL_W - 10 - 84, SL_BTN_Y, 84, 24, COL_BTN, "CLOSE");
}

static void touch_slots(int x, int y, bool saving) {
    if (!in_rect(x, y, SL_X, SL_Y, SL_W, SL_H)) { ui = -1; return; }
    if (in_rect(x, y, SL_X + SL_W - 10 - 84, SL_BTN_Y, 84, 24)) { ui = -1; return; }
    for (int s = 0; s < STORE_NSLOTS; s++) {
        if (!in_rect(x, y, tile_x(s), tile_y(s), TILE_W, TILE_H)) continue;
        char m[48];
        if (saving) {
            bool ok = store_save_slot(s);
            snprintf(m, sizeof m, ok ? "Saved to slot %d" : "Couldn't save slot %d", s + 1);
            flash(m);
            if (ok) last_slot = s;
        } else if (cache_ok[s]) {
            if (store_load_slot(s)) { last_slot = s; ui = -1; }
            else { snprintf(m, sizeof m, "Couldn't load slot %d", s + 1); flash(m); }
        }
        return;
    }
}

// ---- SONG: chain of steps (slot x bars) ----

#define SG_X   10
#define SG_W   300
#define SG_H   184
#define SG_Y   ((240 - SG_H) / 2)
#define STEP_SZ 32
static float step_x(int i) { return SG_X + 10 + (i % 8) * 36; }
static float step_y(int i) { return SG_Y + 32 + (i / 8) * 38; }
#define SG_CTRL_Y (SG_Y + 112)
#define SG_BTN_Y  (SG_Y + SG_H - 32)
#define TOG_X     (SG_X + SG_W - 110)

static void stepper(float x, float y, const char *label, int value, const char *fmt) {
    text(x, y + 4, 0.4f, COL_DIM, 0, "%s", label);
    button(x + 40, y, 26, 24, COL_BTN, "-");                    // [-] value [+], 126 px wide
    text(x + 40 + 26 + 17, y + 4, 0.42f, COL_FG, C2D_AlignCenter, fmt, value);
    button(x + 40 + 60, y, 26, 24, COL_BTN, "+");
}

static void draw_song(void) {
    panel(SG_X, SG_Y, SG_W, SG_H, "SONG");
    // Song mode toggle
    for (int s = 0; s < 2; s++) {
        bool lit = (s == 1) == song_on;
        C2D_DrawRectSolid(TOG_X + s * 50 + 1, SG_Y + 4, 0.5f, 48, 20,
                          lit ? (s ? COL_ACCENT : COL_BTN) : C2D_Color32(24, 29, 35, 255));
        rect_outline(TOG_X + s * 50 + 1, SG_Y + 4, 48, 20, COL_LINE);
        text(TOG_X + s * 50 + 25, SG_Y + 6, 0.38f, lit ? COL_FG : COL_DIM, C2D_AlignCenter, s ? "ON" : "OFF");
    }
    text(TOG_X - 8, SG_Y + 8, 0.34f, COL_DIM, C2D_AlignRight, "song mode");

    int playing_step = song_on && seq_is_playing() ? cur : -1;
    for (int i = 0; i < nsteps; i++) {
        float x = step_x(i), y = step_y(i);
        bool used = cache_ok[steps[i].slot];
        C2D_DrawRectSolid(x, y, 0.5f, STEP_SZ, STEP_SZ, i == playing_step ? C2D_Color32(30, 90, 76, 255) : COL_PANEL2);
        rect_outline(x, y, STEP_SZ, STEP_SZ, i == sel_step ? COL_FG : COL_LINE);
        text(x + STEP_SZ / 2, y + 1, 0.42f, used ? COL_FG : C2D_Color32(240, 90, 90, 255), C2D_AlignCenter, "%d", steps[i].slot + 1);
        text(x + STEP_SZ / 2, y + 17, 0.3f, COL_DIM, C2D_AlignCenter, "x%d", steps[i].bars);
    }
    if (nsteps < SONG_MAX) {                                     // "+" adds a step
        float x = step_x(nsteps), y = step_y(nsteps);
        rect_outline(x, y, STEP_SZ, STEP_SZ, COL_LINE);
        text(x + STEP_SZ / 2, y + 5, 0.5f, COL_DIM, C2D_AlignCenter, "+");
    }

    stepper(SG_X + 10, SG_CTRL_Y, "SLOT", steps[sel_step].slot + 1, "%d");
    stepper(SG_X + 152, SG_CTRL_Y, "BARS", steps[sel_step].bars, "%d");
    if (!cache_ok[steps[sel_step].slot])
        text(SG_X + 10, SG_CTRL_Y + 26, 0.3f, C2D_Color32(240, 90, 90, 255), 0, "slot %d is empty: save into it first", steps[sel_step].slot + 1);

    button(SG_X + 10, SG_BTN_Y, 90, 24, nsteps > 1 ? C2D_Color32(150, 50, 50, 255) : COL_PANEL2, "DEL STEP");
    button(SG_X + SG_W - 10 - 84, SG_BTN_Y, 84, 24, COL_BTN, "CLOSE");
}

static void song_changed(void) { song_save(); }

static void touch_song(int x, int y) {
    if (!in_rect(x, y, SG_X, SG_Y, SG_W, SG_H)) { ui = -1; return; }
    if (in_rect(x, y, SG_X + SG_W - 10 - 84, SG_BTN_Y, 84, 24)) { ui = -1; return; }

    if (in_rect(x, y, TOG_X, SG_Y + 4, 100, 20)) {               // song mode OFF / ON
        bool want = x >= TOG_X + 50;
        if (want != song_on) {
            SONG_LOCK(); song_on = want; SONG_UNLOCK();
            song_changed();
            Pattern p;
            if (want && seq_is_playing() && song_on_play(&p)) seq_set_pattern(&p);   // jump to step 1 now
        }
        return;
    }
    for (int i = 0; i <= nsteps && i < SONG_MAX; i++) {
        if (!in_rect(x, y, step_x(i), step_y(i), STEP_SZ, STEP_SZ)) continue;
        if (i == nsteps) {                                       // add: copy the selected step
            SONG_LOCK();
            steps[nsteps] = steps[sel_step];
            nsteps++;
            SONG_UNLOCK();
            sel_step = nsteps - 1;
            song_changed();
        } else sel_step = i;
        return;
    }
    if (in_rect(x, y, SG_X + 10, SG_BTN_Y, 90, 24) && nsteps > 1) {   // delete selected step
        SONG_LOCK();
        for (int i = sel_step; i < nsteps - 1; i++) steps[i] = steps[i + 1];
        nsteps--;
        if (cur >= nsteps) { cur = 0; bars_done = 0; }
        SONG_UNLOCK();
        if (sel_step >= nsteps) sel_step = nsteps - 1;
        song_changed();
        return;
    }
    // SLOT / BARS steppers
    float sx[2] = { SG_X + 10 + 40, SG_X + 152 + 40 };
    for (int k = 0; k < 2; k++) {
        int d = 0;
        if (in_rect(x, y, sx[k], SG_CTRL_Y, 26, 24)) d = -1;
        else if (in_rect(x, y, sx[k] + 60, SG_CTRL_Y, 26, 24)) d = 1;
        if (!d) continue;
        SONG_LOCK();
        if (k == 0) steps[sel_step].slot = (steps[sel_step].slot + d + STORE_NSLOTS) % STORE_NSLOTS;
        else { int b = steps[sel_step].bars + d; steps[sel_step].bars = b < 1 ? 1 : b > 64 ? 64 : b; }
        SONG_UNLOCK();
        song_changed();
        return;
    }
}

void store_ui_draw(void) {
    if (ui == STORE_UI_SAVE) draw_slots(true);
    else if (ui == STORE_UI_LOAD) draw_slots(false);
    else if (ui == STORE_UI_SONG) draw_song();
}

void store_ui_touch_down(int x, int y) {
    if (ui == STORE_UI_SAVE) touch_slots(x, y, true);
    else if (ui == STORE_UI_LOAD) touch_slots(x, y, false);
    else if (ui == STORE_UI_SONG) touch_song(x, y);
}
