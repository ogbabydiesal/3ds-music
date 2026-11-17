// Renders the eucseq SYNTH page to PNG on macOS for documentation.
//
// It compiles the app's real page_synth.c + synth.c against a tiny citro2d
// shim implemented with CoreGraphics/CoreText (Arial), then drives the page
// with simulated stylus taps/drags to set up each scene. The header and bottom
// bar are redrawn here to match main.c.
//
//   make -C tools/ui_render        -> docs/screenshots/synth-*.png

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <ImageIO/ImageIO.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "modseq.h"
#include "netcfg.h"
#include "page_synth.h"
#include "settings.h"
#include "store.h"
#include "synth.h"
#include "ui.h"

#define SCALE 3

static CGContextRef ctx;

// ------------------------------------------------------------- citro2d shim ---

static void set_fill(u32 c) {
    CGContextSetRGBFillColor(ctx, (c & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0,
                             ((c >> 16) & 0xFF) / 255.0, ((c >> 24) & 0xFF) / 255.0);
}

bool C2D_DrawRectSolid(float x, float y, float z, float w, float h, u32 clr) {
    (void)z;
    set_fill(clr);
    CGContextFillRect(ctx, CGRectMake(x, y, w, h));
    return true;
}

bool C2D_DrawCircleSolid(float x, float y, float z, float rad, u32 clr) {
    (void)z;
    set_fill(clr);
    CGContextFillEllipseInRect(ctx, CGRectMake(x - rad, y - rad, rad * 2, rad * 2));
    return true;
}

// ---- stand-ins for the 3DS network / motion APIs used by settings.c ----
NetConfig net_cfg = { "192.168.1.173", 9000, "/3ds" };
volatile unsigned long net_sent = 0, net_errors = 0;
void net_send(const char *a, const char *t, ...) { (void)a; (void)t; }
void net_edit_host(void) {}
void net_edit_port(void) {}
bool keyboard_number(const char *h, float i, float *o) { (void)h; (void)i; (void)o; return false; }
const char *net_local_ip(void) { return "192.168.1.160"; }
Result HIDUSER_EnableAccelerometer(void) { return 0; }
Result HIDUSER_DisableAccelerometer(void) { return 0; }
Result HIDUSER_EnableGyroscope(void) { return 0; }
Result HIDUSER_DisableGyroscope(void) { return 0; }
// ---- stand-ins for main.c's sequencer hooks used by store.c ----
static Pattern fake_pat = { 120, { {4,4,0,0}, {3,3,0,0}, {8,5,0,0}, {0,0,0,0}, {0,0,0,0} } };
void seq_get_pattern(Pattern *o) { *o = fake_pat; }
void seq_set_pattern(const Pattern *p) { fake_pat = *p; }
bool seq_is_playing(void) { return true; }

// plausible resting values for the screenshot: lying flat, slight hand movement
void hidAccelRead(accelVector *v) { v->x = 18; v->y = -31; v->z = 509; }
void hidGyroRead(angularRate *g) { g->x = 4; g->y = -2; g->z = 1; }

bool C2D_DrawLine(float x0, float y0, u32 c0, float x1, float y1, u32 c1, float th, float d) {
    (void)c1; (void)d;
    CGContextSetRGBStrokeColor(ctx, (c0 & 0xFF) / 255.0, ((c0 >> 8) & 0xFF) / 255.0,
                               ((c0 >> 16) & 0xFF) / 255.0, ((c0 >> 24) & 0xFF) / 255.0);
    CGContextSetLineWidth(ctx, th);
    CGContextMoveToPoint(ctx, x0, y0);
    CGContextAddLineToPoint(ctx, x1, y1);
    CGContextStrokePath(ctx);
    return true;
}

// ------------------------------------------------- ui.h (as in main.c) ---

C2D_TextBuf tbuf = NULL;
C2D_Font    ui_font = NULL;

const char *track_names[5] = { "CUBE", "SPHERE", "PYRAMID", "DIAMOND", "CYLINDER" };
const u8 track_rgb[5][3] = { {235,45,45}, {250,210,35}, {45,105,250}, {40,205,90}, {225,65,225} };

u32 track_color(int t, float flash, bool muted) {
    const u8 *c = track_rgb[t];
    float k = muted ? 0.22f : 1.0f;
    return C2D_Color32((u8)(c[0] * k + (255 - c[0]) * flash), (u8)(c[1] * k + (255 - c[1]) * flash),
                       (u8)(c[2] * k + (255 - c[2]) * flash), 255);
}

void rect_outline(float x, float y, float w, float h, u32 col) {
    C2D_DrawRectSolid(x, y, 0.5f, w, 1, col);
    C2D_DrawRectSolid(x, y + h - 1, 0.5f, w, 1, col);
    C2D_DrawRectSolid(x, y, 0.5f, 1, h, col);
    C2D_DrawRectSolid(x + w - 1, y, 0.5f, 1, h, col);
}

// The app's bcfnt is Arial at 24px; C2D draws text from the top of the line.
void text(float x, float y, float scale, u32 col, u32 align, const char *fmt, ...) {
    char s[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);

    CFStringRef str = CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
    CTFontRef font = CTFontCreateWithName(CFSTR("ArialMT"), 24.0 * scale, NULL);
    CGColorRef c = CGColorCreateGenericRGB((col & 0xFF) / 255.0, ((col >> 8) & 0xFF) / 255.0,
                                           ((col >> 16) & 0xFF) / 255.0, ((col >> 24) & 0xFF) / 255.0);
    const void *keys[] = { kCTFontAttributeName, kCTForegroundColorAttributeName };
    const void *vals[] = { font, c };
    CFDictionaryRef attr = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, str, attr);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CGFloat asc, desc, lead;
    double w = CTLineGetTypographicBounds(line, &asc, &desc, &lead);
    double dx = (align & C2D_AlignCenter) == C2D_AlignCenter ? x - w / 2
              : (align & C2D_AlignRight) ? x - w : x;
    CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1, -1));
    CGContextSetTextPosition(ctx, dx, y + asc);
    CTLineDraw(line, ctx);
    CFRelease(line); CFRelease(as); CFRelease(attr); CGColorRelease(c); CFRelease(font); CFRelease(str);
}

// ------------------------------------------- header + bottom bar (main.c) ---

static void draw_button(float x, float y, float w, float h, u32 fill, const char *label, float sc) {
    C2D_DrawRectSolid(x, y, 0.5f, w, h, fill);
    text(x + w / 2, y + (h - 30 * sc) / 2, sc, COL_FG, C2D_AlignCenter, "%s", label);
}

static int scene_page = 1;                   // 1 = SYNTH, 2 = STEPS

static void draw_chrome(bool playing) {
    text(6, 4, 0.48f, COL_FG, 0, scene_page == 2 ? "STEPS" : "SYNTH");
    C2D_DrawRectSolid(288, 2, 0.5f, 28, 19, COL_BTN);                 // settings gear
    draw_gear(302, 11.5f, settings_osc_on() ? COL_ACCENT : COL_FG, COL_BTN);
    draw_button(96, 2, 58, 19, COL_BTN, "SAVE", 0.42f);
    draw_button(158, 2, 58, 19, COL_BTN, "LOAD", 0.42f);
    draw_button(220, 2, 62, 19, song_mode_on() ? COL_ACCENT : COL_BTN, "SONG", 0.42f);

    float by = 204, bh = 32;
    draw_button(4, by, 74, bh, playing ? C2D_Color32(190, 60, 60, 255) : COL_ACCENT, playing ? "STOP" : "PLAY", 0.55f);
    C2D_DrawRectSolid(82, by, 0.5f, 138, bh, COL_PANEL);
    draw_button(84, by + 3, 34, bh - 6, COL_BTN, "-", 0.7f);
    draw_button(184, by + 3, 34, bh - 6, COL_BTN, "+", 0.7f);
    text(151, by + 3, 0.6f, COL_FG, C2D_AlignCenter, "120");
    text(151, by + 19, 0.34f, COL_DIM, C2D_AlignCenter, "BPM");
    draw_button(224, by, 45, bh, COL_BTN, "<", 0.6f);
    draw_button(271, by, 45, bh, COL_BTN, ">", 0.6f);
    for (int i = 0; i < 3; i++)
        C2D_DrawRectSolid(268 - 9 + i * 6 + 2, by + bh - 5, 0.5f, 3, 3, i == scene_page ? COL_FG : COL_LINE);
}

// ------------------------------------------------------------ rendering ---

static void render(const char *path, bool playing) {
    int W = BOT_W * SCALE, H = 240 * SCALE;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    ctx = CGBitmapContextCreate(NULL, W, H, 8, 0, cs, kCGImageAlphaPremultipliedLast);
    CGContextTranslateCTM(ctx, 0, H);
    CGContextScaleCTM(ctx, SCALE, -SCALE);          // 3DS coordinates: origin top-left, y down
    CGContextSetShouldAntialias(ctx, true);

    C2D_DrawRectSolid(0, 0, 0.5f, BOT_W, 240, COL_BG_BOT);
    if (scene_page == 2) mod_page_draw(); else synth_page_draw();
    draw_chrome(playing);
    synth_page_draw_popup();
    settings_draw();
    store_ui_draw();

    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(dst, img, NULL);
    CGImageDestinationFinalize(dst);
    CFRelease(dst); CFRelease(url); CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs);
    printf("wrote %s\n", path);
}

// Stylus helpers
static void tap(int x, int y) { synth_page_touch(true, true, false, x, y); synth_page_touch(false, false, true, x, y); }
static void press(int x, int y) { synth_page_touch(true, true, false, x, y); }
static void move(int x, int y) { synth_page_touch(false, true, false, x, y); }
static void release_at(int x, int y) { synth_page_touch(false, false, true, x, y); }
static void drag(int x0, int y0, int x1, int y1) {
    press(x0, y0);
    for (int i = 1; i <= 10; i++) move(x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10);
    release_at(x1, y1);
}

// Screen centre of a node at the current (100%, unpanned) view
static int ncx(int i) { return 4 + (int)syn_nodes[i].x + 32; }
static int ncy(int i) { return 73 + (int)syn_nodes[i].y + 15; }

int main(int argc, char **argv) {
    char out[1024];
    if (!realpath(argc > 1 ? argv[1] : ".", out)) return 1;
    mkdir("build", 0777);
    chdir("build");                              // settings.c / store.c write files; keep them inside build/
    mkdir("store", 0777);
    unlink("store/song.txt");
    for (int i = 1; i <= 8; i++) { char f[32]; snprintf(f, sizeof f, "store/slot%d.txt", i); unlink(f); }
    char p[1200];
#define OUT(name) (snprintf(p, sizeof p, "%s/%s", out, name), p)

    store_init();
    syn_init();
    mod_init();                                  // starter: NOISE(0) -> ENV(1), ENV out on shape 1
    render(OUT("synth-1-starter.png"), false);

    // ENV settings popup
    tap(ncx(1), ncy(1));
    render(OUT("synth-2-env-settings.png"), false);
    tap(2, 2);                                   // tap outside closes

    // Kick drum chain: ENV (pitch) -> OSC sine -> ENV (amp), out on shapes 2 + 4
    int pe = syn_add_node(NODE_ENV, 24, 84);
    int os = syn_add_node(NODE_OSC, 130, 84);
    int ae = syn_add_node(NODE_ENV, 236, 84);
    syn_connect(pe, os);
    syn_connect(os, ae);
    syn_set_param(pe, ENV_D, 45);  syn_set_param(pe, ENV_LEVEL, 1);  syn_set_param(pe, ENV_LEN, 200);
    syn_set_param(os, OSC_FREQ, 55); syn_set_param(os, OSC_MOD, 3);   syn_set_param(os, OSC_LEVEL, 0.9f);
    syn_set_param(ae, ENV_D, 320); syn_set_param(ae, ENV_LEN, 260);
    syn_toggle_output(ae, 1);
    syn_toggle_output(ae, 3);
    syn_toggle_output(1, 2);                     // hi-hat env also on shape 3
    render(OUT("synth-3-two-instruments.png"), true);

    // OSC with a hand-drawn custom wave
    syn_set_param(os, OSC_WAVE, WAVE_CUSTOM);
    const float wave[SYN_WT_SIZE] = { 0, .55f, .9f, 1, .8f, .35f, -.1f, -.2f, .1f, .3f, 0, -.45f, -.85f, -1, -.7f, -.3f };
    for (int i = 0; i < SYN_WT_SIZE; i++) syn_set_wave_point(os, i, wave[i]);
    tap(ncx(os), ncy(os));
    render(OUT("synth-4-osc-custom-wave.png"), true);
    tap(2, 2);
    syn_set_param(os, OSC_WAVE, WAVE_SINE);

    // Box select in progress around the kick chain
    press(14, 150);
    for (int i = 1; i <= 10; i++) move(14 + 300 * i / 10, 150 + 46 * i / 10);
    render(OUT("synth-5-box-select.png"), false);
    release_at(314, 196);

    // Copy, paste, move the pasted copy down, then zoom out to see everything
    tap(179, 61);                                // COPY
    tap(234, 61);                                // PASTE (pasted nodes become the selection)
    int pasted = -1;
    for (int i = SYN_MAX_NODES - 1; i >= 0 && pasted < 0; i--)
        if (syn_nodes[i].used && syn_nodes[i].type == NODE_ENV && syn_nodes[i].input >= 0) pasted = i;
    drag(ncx(pasted), ncy(pasted), ncx(pasted), ncy(pasted) + 60);
    syn_toggle_output(pasted, 4);                // the copy plays on the cylinder
    syn_set_param(syn_nodes[syn_nodes[pasted].input].input >= 0 ? syn_nodes[pasted].input : pasted, OSC_FREQ, 110);
    tap(105, 61);                                // tap the % button: fit everything
    render(OUT("synth-6-copy-paste-zoom.png"), false);

    // REVERB: starter NOISE -> ENV -> REVERB, the reverb is now the output; carousel scrolled to show it
    syn_init();
    int rv = syn_add_node(NODE_REVERB, 236, 40);
    syn_connect(1, rv);
    syn_clear_outputs(1);
    syn_toggle_output(rv, 0);
    syn_set_param(rv, REV_T60, 2.5f);
    syn_set_param(rv, REV_MIX, 0.4f);
    tap(105, 61);                                // % : fit
    tap(BOT_W - 10, 36);                         // carousel > (shows + REVERB)
    render(OUT("synth-7-reverb.png"), false);
    synth_page_open_popup(rv);
    render(OUT("synth-8-reverb-settings.png"), false);
    tap(2, 2);

    // BEADS: OSC -> ENV -> BEADS -> REVERB, frozen texture with pitch down a fifth
    syn_init();
    syn_delete_node(0); syn_delete_node(1);
    int bo = syn_add_node(NODE_OSC, 24, 30), be = syn_add_node(NODE_ENV, 120, 30);
    int bb = syn_add_node(NODE_BEADS, 120, 84), br = syn_add_node(NODE_REVERB, 216, 84);
    syn_connect(bo, be); syn_connect(be, bb); syn_connect(bb, br);
    syn_toggle_output(br, 0);
    syn_set_param(bo, OSC_WAVE, WAVE_SAW);
    syn_set_param(bb, BD_PITCH, -7); syn_set_param(bb, BD_SIZE, 450); syn_set_param(bb, BD_DENSITY, 0.55f);
    syn_set_param(bb, BD_RANDOM, 0.35f); syn_set_param(bb, BD_FEEDBACK, 0.3f); syn_set_param(bb, BD_FREEZE, 1);
    syn_set_param(br, REV_T60, 4); syn_set_param(br, REV_MIX, 0.35f);
    tap(BOT_W - 10, 36);                         // carousel > > (shows + BEADS)
    tap(105, 61);                                // % : fit
    render(OUT("synth-9-beads.png"), false);
    synth_page_open_popup(bb);
    render(OUT("synth-10-beads-settings.png"), false);
    tap(2, 2);
    tap(10, 36); tap(10, 36);                    // carousel back

    // Settings popup (gear): default, then OSC + gyro turned on
    settings_open();
    render(OUT("settings-1-default.png"), false);
    settings_touch_down(260, 49);                // OSC OUTPUT -> ON
    settings_touch_down(260, 127);               // GYRO  -> ON
    settings_touch_down(260, 153);               // ACCEL -> ON
    render(OUT("settings-2-osc-on.png"), false);
    settings_close();

    // SAVE: three slots with different patterns, then the LOAD view
    Pattern pats[3] = {
        { 120, { {4,4,0,0}, {3,3,0,0}, {8,5,0,0}, {0,0,0,0}, {0,0,0,0} } },
        { 96,  { {4,4,0,0}, {0,0,0,0}, {8,5,0,0}, {6,3,0,0}, {0,0,0,0} } },
        { 132, { {8,8,0,0}, {5,5,0,0}, {16,7,0,1}, {3,3,0,0}, {2,1,0,0} } },
    };
    for (int i = 0; i < 3; i++) { seq_set_pattern(&pats[i]); store_save_slot(i); }
    store_ui_open(STORE_UI_SAVE);
    store_ui_touch_down(16 + 10 + 2 * 68 + 20, (240 - 172) / 2 + 28 + 20);    // save into slot 3 -> message
    render(OUT("store-1-save.png"), false);
    store_ui_close();

    // SONG: 1 x4, 2 x2, 1 x4, 3 x8 with song mode on
    store_ui_open(STORE_UI_SONG);
    int sgy = (240 - 184) / 2;
    store_ui_touch_down(10 + 300 - 110 + 75, sgy + 10);                       // song mode ON
    int adds[3][2] = { {2, 2}, {1, 4}, {3, 8} };                              // slot, bars for steps 2..4
    for (int k = 0; k < 3; k++) {
        store_ui_touch_down(10 + 10 + (k + 1) * 36 + 5, sgy + 32 + 5);        // "+" (copies the selected step)
        int slot_now = k == 0 ? 1 : adds[k - 1][0], bars_now = k == 0 ? 4 : adds[k - 1][1];
        while (slot_now != adds[k][0]) { store_ui_touch_down(10 + 10 + 40 + 60 + 5, sgy + 112 + 5); slot_now = slot_now % 8 + 1; }
        while (bars_now < adds[k][1]) { store_ui_touch_down(10 + 152 + 40 + 60 + 5, sgy + 112 + 5); bars_now++; }
        while (bars_now > adds[k][1]) { store_ui_touch_down(10 + 152 + 40 + 5, sgy + 112 + 5); bars_now--; }
    }
    Pattern sp;
    song_on_play(&sp);
    for (int b = 0; b < 5; b++) song_on_bar(&sp);                             // now on step 2
    store_ui_touch_down(10 + 10 + 1 * 36 + 5, sgy + 32 + 5);                  // select step 2
    render(OUT("store-2-song.png"), true);
    store_ui_close();

    // STEPS page: S1 plays a melody on the kick OSC's FREQ (sphere pulses);
    // S2 steps the hi-hat ENV decay (cube pulses). Assignments live in the nodes.
    scene_page = 2;
    ModSeq *S1 = &mod_seqs[0], *S2 = &mod_seqs[1];
    S1->trig = 1; S1->nsteps = 8; S1->min = 55; S1->max = 220;
    const float mel[8] = { 0, .5f, .25f, .75f, 0, 1, .5f, .38f };
    for (int i = 0; i < 8; i++) S1->v[i] = mel[i];
    S2->trig = 0; S2->nsteps = 4; S2->min = 40; S2->max = 300;
    syn_set_mod_src(os, OSC_FREQ, 0);
    syn_set_mod_src(1, ENV_D, 1);
    mod_reset();
    for (int i = 0; i < 3; i++) mod_on_pulse(1);  // now playing step 3
    mod_page_touch(true, true, false, 4 + 5 * 39 + 10, 150);   // touch slider 6 (sets it + readout)
    mod_page_touch(false, false, true, 4 + 5 * 39 + 10, 150);
    render(OUT("steps-1-lane.png"), true);

    // OSC settings with FREQ driven by S1
    scene_page = 1;
    synth_page_open_popup(os);
    render(OUT("steps-2-assign.png"), true);
    tap(2, 2);
    store_ui_close();
    return 0;
}
