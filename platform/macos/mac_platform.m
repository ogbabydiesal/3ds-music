// nodeTrix for macOS: the platform layer.
//
// The app itself is the 3DS code, compiled unchanged against shim/3ds.h and
// shim/citro2d.h. This file implements those calls on the Mac:
//   - one window showing both screens (top screen above, bottom screen below),
//     drawn with CoreGraphics / CoreText (Arial, as on the 3DS);
//   - the mouse is the stylus on the bottom screen; dragging on the top screen
//     turns the view (the circle pad); keys stand in for the buttons;
//   - CoreAudio output at 32 kHz stereo, and the microphone for MIC nodes;
//   - the system keyboard is a small text dialog;
//   - "sdmc:/3ds/nodeTrix" is ~/Library/Application Support/nodeTrix.
// The app's main() runs on its own thread (as nodetrix_main); Cocoa keeps the main thread.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <AudioToolbox/AudioToolbox.h>
#include <ImageIO/ImageIO.h>
#include <CoreText/CoreText.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <mach/mach_time.h>
#include <net/if.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>

#include "3ds.h"
#include "citro2d.h"
#include "synth.h"

int nodetrix_main(void);

// Logical layout: the 400x240 top screen, the 320x240 bottom screen centred under it.
#define CANVAS_W 400
#define CANVAS_H 480
#define BOT_X    40
#define BOT_Y    240

// ------------------------------------------------------------------ paths ---

static char app_dir[1024];

static void mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static void paths_init(void) {
    const char *home = getenv("HOME");
    snprintf(app_dir, sizeof app_dir, "%s/Library/Application Support/nodeTrix", home ? home : "/tmp");
    mkdirs(app_dir);
    char p[1100];
    snprintf(p, sizeof p, "%s/samples", app_dir);
    mkdir(p, 0755);
}

const char *nt_path(const char *p) {
    static _Thread_local char buf[4][1100];
    static _Thread_local int k = 0;
    static const char app[] = "sdmc:/3ds/nodeTrix", sd[] = "sdmc:/";
    char *o = buf[k = (k + 1) & 3];
    if (!strncmp(p, app, sizeof app - 1)) snprintf(o, sizeof buf[0], "%s%s", app_dir, p + sizeof app - 1);
    else if (!strncmp(p, sd, sizeof sd - 1)) snprintf(o, sizeof buf[0], "%s/.sd/%s", app_dir, p + sizeof sd - 1);
    else return p;
    return o;
}

// ----------------------------------------------------------- time, threads ---

static double ticks_per_mach, mach_per_ns;

u64 svcGetSystemTick(void) { return (u64)(mach_absolute_time() * ticks_per_mach); }

void svcSleepThread(s64 ns) {
    if (ns <= 0) return;
    struct timespec ts = { ns / 1000000000LL, ns % 1000000000LL };
    nanosleep(&ts, NULL);
}

Result svcGetThreadPriority(s32 *out, u32 h) { (void)h; *out = 0x30; return 0; }

struct NtThread { pthread_t th; ThreadFunc fn; void *arg; };

static void *thread_tramp(void *p) {
    struct NtThread *t = p;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    t->fn(t->arg);
    return NULL;
}

Thread threadCreate(ThreadFunc fn, void *arg, size_t stack, int prio, int core, bool detached) {
    (void)prio; (void)core;
    struct NtThread *t = calloc(1, sizeof *t);
    t->fn = fn; t->arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack < 512 * 1024 ? 512 * 1024 : stack);
    if (pthread_create(&t->th, &a, thread_tramp, t)) { free(t); t = NULL; }
    else if (detached) pthread_detach(t->th);
    pthread_attr_destroy(&a);
    return t;
}

Result threadJoin(Thread t, u64 timeout) { (void)timeout; return t ? pthread_join(t->th, NULL) : -1; }
void threadFree(Thread t) { free(t); }

// ------------------------------------------------------------------ system ---

static atomic_bool quit_requested;
bool aptMainLoop(void) { return !atomic_load(&quit_requested); }
void gfxInitDefault(void) {}
void gfxSet3D(bool on) { (void)on; }
void gfxExit(void) {}
float osGet3DSliderState(void) { return 0; }           // one picture, no stereo
Result romfsInit(void) { return 0; }
Result romfsExit(void) { return 0; }
Result socInit(u32 *b, u32 s) { (void)b; (void)s; return 0; }
Result socExit(void) { return 0; }

Result HIDUSER_EnableAccelerometer(void) { return 0; }
Result HIDUSER_DisableAccelerometer(void) { return 0; }
Result HIDUSER_EnableGyroscope(void) { return 0; }
Result HIDUSER_DisableGyroscope(void) { return 0; }
void hidAccelRead(accelVector *v) { v->x = 0; v->y = 0; v->z = 512; }  // lying flat
void hidGyroRead(angularRate *g) { g->x = g->y = g->z = 0; }

long nt_gethostid(void) {
    struct ifaddrs *list, *i;
    long ip = 0;
    if (getifaddrs(&list)) return 0;
    for (i = list; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !(i->ifa_flags & IFF_LOOPBACK) &&
            (i->ifa_flags & IFF_UP)) {
            ip = (long)((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
            break;
        }
    freeifaddrs(list);
    return ip;
}

// ------------------------------------------------------------------- input ---
// Written by the Cocoa thread, read once per frame by hidScanInput (app thread).

static os_unfair_lock in_lock = OS_UNFAIR_LOCK_INIT;
static u32   in_keys;                    // keys held on the keyboard
static bool  in_mouse_down;              // stylus on the bottom screen
static bool  in_click_pending;           // a press not yet seen by a frame
static bool  in_release_next;            // that press was already released
static int   in_tx, in_ty;               // stylus position (bottom-screen pixels)
static float in_drag_dx, in_drag_dy;     // top-screen drag since the last frame

static u32 k_held, k_prev, k_repeat;
static u32 rep_delay = 20, rep_interval = 4, rep_count[32];
static touchPosition cur_touch;
static circlePosition cur_circle;

void hidScanInput(void) {
    os_unfair_lock_lock(&in_lock);
    bool touch;
    if (in_click_pending) {                         // every click is held for at least one frame
        touch = true;
        in_click_pending = false;
        in_release_next = !in_mouse_down;
    } else if (in_release_next) {
        touch = false;
        in_release_next = false;
    } else {
        touch = in_mouse_down;
    }
    u32 keys = in_keys;
    cur_touch.px = (u16)in_tx;
    cur_touch.py = (u16)in_ty;
    // Mouse pixels -> circle pad units (main.c turns |d| > 15 into 0.035 rad per 156)
    float dx = in_drag_dx * 45.0f, dy = -in_drag_dy * 45.0f;
    in_drag_dx = in_drag_dy = 0;
    os_unfair_lock_unlock(&in_lock);

    cur_circle.dx = (s16)fmaxf(-2000, fminf(2000, dx));
    cur_circle.dy = (s16)fmaxf(-2000, fminf(2000, dy));
    k_prev = k_held;
    k_held = keys | (touch ? KEY_TOUCH : 0);
    k_repeat = 0;
    for (int b = 0; b < 32; b++) {
        if (!(k_held & BIT(b))) { rep_count[b] = 0; continue; }
        u32 c = rep_count[b]++;
        if (c == 0 || (c >= rep_delay && (c - rep_delay) % rep_interval == 0)) k_repeat |= BIT(b);
    }
}

u32 hidKeysDown(void) { return k_held & ~k_prev; }
u32 hidKeysHeld(void) { return k_held; }
u32 hidKeysUp(void) { return k_prev & ~k_held; }
u32 hidKeysDownRepeat(void) { return k_repeat; }
void hidSetRepeatParameters(u32 d, u32 i) { rep_delay = d; rep_interval = i ? i : 1; }
void hidTouchRead(touchPosition *tp) { *tp = cur_touch; }
void hidCircleRead(circlePosition *cp) { *cp = cur_circle; }

static void touch_cancel(void) {
    os_unfair_lock_lock(&in_lock);
    in_mouse_down = false;
    in_release_next = true;
    os_unfair_lock_unlock(&in_lock);
}

// ------------------------------------------------------------------ drawing ---

struct NtTarget { int screen, side; };
static C3D_RenderTarget targets[3] = { { GFX_TOP, GFX_LEFT }, { GFX_TOP, GFX_RIGHT }, { GFX_BOTTOM, GFX_LEFT } };

static CGContextRef ctx;
static int ctx_w, ctx_h;                 // pixels
static atomic_int view_px_w = 800;       // the window's drawable width in pixels (Cocoa thread)
static bool in_scene, discard;
static NSView *game_view;

bool C3D_Init(size_t n) { (void)n; return true; }
void C3D_Fini(void) {}
bool C2D_Init(size_t n) { (void)n; return true; }
void C2D_Fini(void) {}
void C2D_Prepare(void) {}

C3D_RenderTarget *C2D_CreateScreenTarget(gfxScreen_t s, gfx3dSide_t side) {
    return s == GFX_BOTTOM ? &targets[2] : &targets[side == GFX_RIGHT ? 1 : 0];
}

static void set_fill(u32 c) {
    CGContextSetRGBFillColor(ctx, (c & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0,
                             ((c >> 16) & 0xFF) / 255.0, ((c >> 24) & 0xFF) / 255.0);
}

static void screen_rect(const C3D_RenderTarget *t, CGRect *r) {
    *r = t->screen == GFX_BOTTOM ? CGRectMake(BOT_X, BOT_Y, 320, 240) : CGRectMake(0, 0, 400, 240);
}

bool C3D_FrameBegin(u8 flags) {
    (void)flags;
    int pw = atomic_load(&view_px_w);
    if (pw < CANVAS_W) pw = CANVAS_W;
    if (pw > 4 * CANVAS_W) pw = 4 * CANVAS_W;
    int ph = pw * CANVAS_H / CANVAS_W;
    if (!ctx || pw != ctx_w || ph != ctx_h) {
        if (ctx) CGContextRelease(ctx);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        ctx = CGBitmapContextCreate(NULL, pw, ph, 8, 0, cs, kCGImageAlphaNoneSkipLast);
        CGColorSpaceRelease(cs);
        ctx_w = pw; ctx_h = ph;
        CGContextTranslateCTM(ctx, 0, ph);
        CGContextScaleCTM(ctx, (double)pw / CANVAS_W, -(double)ph / CANVAS_H);   // origin top-left, y down
        CGContextSetShouldAntialias(ctx, true);
        CGContextSetLineCap(ctx, kCGLineCapRound);
    }
    CGContextSetRGBFillColor(ctx, 0.02, 0.025, 0.035, 1);
    CGContextFillRect(ctx, CGRectMake(0, 0, CANVAS_W, CANVAS_H));
    in_scene = false;
    discard = false;
    return true;
}

void C2D_TargetClear(C3D_RenderTarget *t, u32 color) {
    if (t->side == GFX_RIGHT) return;                  // the right eye isn't shown
    CGRect r;
    screen_rect(t, &r);
    if (in_scene) { CGContextRestoreGState(ctx); in_scene = false; }
    set_fill(color);
    CGContextFillRect(ctx, r);
}

void C2D_SceneBegin(C3D_RenderTarget *t) {
    if (in_scene) { CGContextRestoreGState(ctx); in_scene = false; }
    discard = t->side == GFX_RIGHT;
    if (discard) return;
    CGRect r;
    screen_rect(t, &r);
    CGContextSaveGState(ctx);
    in_scene = true;
    CGContextClipToRect(ctx, r);
    CGContextTranslateCTM(ctx, r.origin.x, r.origin.y);
}

static u64 next_frame;

// ---- self-test: NT_SELFTEST=<folder> runs the app with no window and no sound,
// taps through the pages, saves each one as a PNG and checks that the synth
// makes sound. Used by `make test` and CI.
static const char *selftest_dir;
static int selftest_frame;
static float selftest_peak;
static int selftest_fail;

static void selftest_png(const char *name) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", selftest_dir, name);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(d, img, NULL);
    if (!CGImageDestinationFinalize(d)) selftest_fail++;
    CFRelease(d); CFRelease(url); CGImageRelease(img);
    printf("wrote %s\n", path);
}

static void selftest_tap(int x, int y) {          // a stylus tap on the bottom screen
    os_unfair_lock_lock(&in_lock);
    in_tx = x; in_ty = y;
    in_mouse_down = false;
    in_click_pending = true;
    os_unfair_lock_unlock(&in_lock);
}

static void selftest_keys(u32 k) {
    os_unfair_lock_lock(&in_lock);
    in_keys = k;
    os_unfair_lock_unlock(&in_lock);
}

static void selftest_step(void) {
    int f = ++selftest_frame;
    if (f == 5) selftest_keys(KEY_A);                 // play
    if (f == 6) selftest_keys(0);
    if (f >= 6) {                                     // pull 1/60 s of audio through the synth
        int16_t buf[2 * 534];
        syn_render(buf, 534);
        for (int i = 0; i < 2 * 534; i++) {
            float a = fabsf(buf[i] / 32768.0f);
            if (a > selftest_peak) selftest_peak = a;
        }
    }
    if (f == 40) selftest_png("mac-1-sequencer.png");
    if (f == 41) selftest_tap(293, 220);              // > : SYNTH page
    if (f == 70) selftest_png("mac-2-synth.png");
    if (f == 71) selftest_tap(293, 220);              // > : STEPS page
    if (f == 100) selftest_png("mac-3-steps.png");
    if (f == 101) selftest_keys(KEY_SELECT);          // settings popup
    if (f == 102) selftest_keys(0);
    if (f == 120) selftest_png("mac-4-settings.png");
    if (f == 121) {
        printf("audio peak %.3f over %d frames: %s\n", selftest_peak, f - 6, selftest_peak > 0.01f ? "PASS" : "FAIL");
        if (selftest_peak <= 0.01f) selftest_fail++;
        atomic_store(&quit_requested, true);
    }
}

void C3D_FrameEnd(u8 flags) {
    (void)flags;
    if (in_scene) { CGContextRestoreGState(ctx); in_scene = false; }
    if (selftest_dir) { selftest_step(); return; }    // as fast as possible, nothing on screen
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    dispatch_async(dispatch_get_main_queue(), ^{
        game_view.layer.contents = (__bridge id)img;
        CGImageRelease(img);
    });
    // ~60 frames a second, like the 3DS
    u64 now = mach_absolute_time(), frame = (u64)(1e9 / 60.0 * mach_per_ns);
    if (!next_frame || now > next_frame + 4 * frame) next_frame = now;
    next_frame += frame;
    mach_wait_until(next_frame);
}

bool C2D_DrawRectSolid(float x, float y, float z, float w, float h, u32 clr) {
    (void)z;
    if (discard) return true;
    set_fill(clr);
    CGContextFillRect(ctx, CGRectMake(x, y, w, h));
    return true;
}

bool C2D_DrawCircleSolid(float x, float y, float z, float rad, u32 clr) {
    (void)z;
    if (discard) return true;
    set_fill(clr);
    CGContextFillEllipseInRect(ctx, CGRectMake(x - rad, y - rad, rad * 2, rad * 2));
    return true;
}

bool C2D_DrawLine(float x0, float y0, u32 c0, float x1, float y1, u32 c1, float th, float d) {
    (void)c1; (void)d;
    if (discard) return true;
    CGContextSetRGBStrokeColor(ctx, (c0 & 0xFF) / 255.0, ((c0 >> 8) & 0xFF) / 255.0,
                               ((c0 >> 16) & 0xFF) / 255.0, ((c0 >> 24) & 0xFF) / 255.0);
    CGContextSetLineWidth(ctx, th);
    CGContextMoveToPoint(ctx, x0, y0);
    CGContextAddLineToPoint(ctx, x1, y1);
    CGContextStrokePath(ctx);
    return true;
}

// ---- text: Arial, as the 3DS build's bcfnt (24 px at scale 1, drawn from the top of the line) ----

C2D_TextBuf C2D_TextBufNew(size_t n) { (void)n; return (C2D_TextBuf)1; }
void C2D_TextBufClear(C2D_TextBuf b) { (void)b; }
void C2D_TextBufDelete(C2D_TextBuf b) { (void)b; }
C2D_Font C2D_FontLoad(const char *path) { (void)path; return NULL; }
void C2D_FontFree(C2D_Font f) { (void)f; }
const char *C2D_TextFontParse(C2D_Text *t, C2D_Font f, C2D_TextBuf b, const char *s) {
    (void)f; (void)b;
    t->str = s;                          // the caller's buffer outlives the draw call
    return NULL;
}
void C2D_TextOptimize(const C2D_Text *t) { (void)t; }

#define NFONTS 32
static struct { int key; CFDictionaryRef attr; CGFloat asc; } fonts[NFONTS];

static CFDictionaryRef font_attr(float size, CGFloat *asc) {
    int key = (int)lroundf(size * 100);
    int slot = -1;
    for (int i = 0; i < NFONTS; i++) {
        if (fonts[i].key == key) { *asc = fonts[i].asc; return fonts[i].attr; }
        if (!fonts[i].key && slot < 0) slot = i;
    }
    CTFontRef font = CTFontCreateWithName(CFSTR("ArialMT"), size, NULL);
    const void *keys[] = { kCTFontAttributeName, kCTForegroundColorFromContextAttributeName };
    const void *vals[] = { font, kCFBooleanTrue };
    CFDictionaryRef attr = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    *asc = CTFontGetAscent(font);
    CFRelease(font);
    if (slot < 0) { slot = key % NFONTS; CFRelease(fonts[slot].attr); }   // full: replace one
    fonts[slot].key = key; fonts[slot].attr = attr; fonts[slot].asc = *asc;
    return attr;
}

void nt_draw_text(const C2D_Text *t, u32 flags, float x, float y, float z, float sx, float sy, u32 color) {
    (void)z; (void)sy;
    if (discard || !t->str || !*t->str) return;
    u32 col = (flags & C2D_WithColor) ? color : 0xFF000000;
    CFStringRef str = CFStringCreateWithCString(NULL, t->str, kCFStringEncodingUTF8);
    if (!str) return;
    CGFloat asc;
    CFDictionaryRef attr = font_attr(24.0f * sx, &asc);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, str, attr);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    double w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    u32 align = flags & (C2D_AlignRight | C2D_AlignCenter);
    double dx = align == C2D_AlignCenter ? x - w / 2 : align == C2D_AlignRight ? x - w : x;
    set_fill(col);
    CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1, -1));
    CGContextSetTextPosition(ctx, dx, y + asc);
    CTLineDraw(line, ctx);
    CFRelease(line); CFRelease(as); CFRelease(str);
}

// ------------------------------------------------------- system keyboard ---

void swkbdInit(SwkbdState *kb, SwkbdType type, int nbuttons, int max_len) {
    (void)nbuttons;
    memset(kb, 0, sizeof *kb);
    kb->type = type;
    kb->max_len = max_len;
}
void swkbdSetNumpadKeys(SwkbdState *kb, int l, int r) { (void)kb; (void)l; (void)r; }
void swkbdSetHintText(SwkbdState *kb, const char *h) { snprintf(kb->hint, sizeof kb->hint, "%s", h ? h : ""); }
void swkbdSetInitialText(SwkbdState *kb, const char *s) { snprintf(kb->initial, sizeof kb->initial, "%s", s ? s : ""); }
void swkbdSetValidation(SwkbdState *kb, int v, u32 f, int m) { (void)kb; (void)v; (void)f; (void)m; }

SwkbdButton swkbdInputText(SwkbdState *kb, char *out, size_t outlen) {
    __block NSString *result = nil;
    NSString *hint = [NSString stringWithUTF8String:kb->hint];
    NSString *initial = [NSString stringWithUTF8String:kb->initial];
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSAlert *a = [NSAlert new];
        a.messageText = hint.length ? hint : @"Enter a value";
        [a addButtonWithTitle:@"OK"];
        [a addButtonWithTitle:@"Cancel"];
        NSTextField *f = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 240, 24)];
        f.stringValue = initial;
        a.accessoryView = f;
        a.window.initialFirstResponder = f;
        if ([a runModal] == NSAlertFirstButtonReturn) result = f.stringValue;
    });
    touch_cancel();                                    // the click that opened the dialog is over
    NSString *trimmed = [result stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (!trimmed.length) return SWKBD_BUTTON_LEFT;
    size_t lim = kb->max_len > 0 && (size_t)kb->max_len + 1 < outlen ? (size_t)kb->max_len + 1 : outlen;
    snprintf(out, lim, "%s", trimmed.UTF8String);
    return SWKBD_BUTTON_CONFIRM;
}

// -------------------------------------------------------------------- audio ---

static AudioComponentInstance out_unit;
static bool audio_ok;

static OSStatus render_cb(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts,
                          UInt32 bus, UInt32 n, AudioBufferList *io) {
    (void)ref; (void)fl; (void)ts; (void)bus;
    syn_render((int16_t *)io->mBuffers[0].mData, (int)n);
    return noErr;
}

static AudioStreamBasicDescription pcm16(int channels) {
    AudioStreamBasicDescription f = { 0 };
    f.mSampleRate = SYN_SAMPLE_RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mChannelsPerFrame = channels;
    f.mBitsPerChannel = 16;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = f.mBytesPerPacket = 2 * channels;
    return f;
}

bool syn_audio_start(void) {
    if (selftest_dir) { audio_ok = true; return true; }   // the self-test pulls audio itself
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                    kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &out_unit)) return false;
    AudioStreamBasicDescription f = pcm16(2);                       // interleaved L,R at 32 kHz
    AURenderCallbackStruct cb = { render_cb, NULL };
    UInt32 frames = 256;
    if (AudioUnitSetProperty(out_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ||
        AudioUnitSetProperty(out_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb)) {
        AudioComponentInstanceDispose(out_unit);
        return false;
    }
    AudioUnitSetProperty(out_unit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &frames, sizeof frames);
    if (AudioUnitInitialize(out_unit) || AudioOutputUnitStart(out_unit)) {
        AudioComponentInstanceDispose(out_unit);
        return false;
    }
    audio_ok = true;
    return true;
}

void syn_audio_stop(void) {
    if (!audio_ok || selftest_dir) return;
    AudioOutputUnitStop(out_unit);
    AudioUnitUninitialize(out_unit);
    AudioComponentInstanceDispose(out_unit);
    audio_ok = false;
}

bool syn_audio_ok(void) { return audio_ok; }

// ---- microphone: an AudioQueue fills a ring; the synth reads it ~25 ms behind ----

#define MIC_RING 16384
static int16_t mic_ring[MIC_RING];
static atomic_uint_fast64_t mic_written;
static AudioQueueRef mic_q;
static double mic_rp;
static bool mic_rp_ok;

static void mic_cb(void *ref, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp *ts,
                   UInt32 npk, const AudioStreamPacketDescription *pd) {
    (void)ref; (void)ts; (void)npk; (void)pd;
    const int16_t *s = b->mAudioData;
    u32 n = b->mAudioDataByteSize / 2;
    u64 w = atomic_load(&mic_written);
    for (u32 i = 0; i < n; i++) mic_ring[(w + i) % MIC_RING] = s[i];
    atomic_store(&mic_written, w + n);
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

bool plat_mic_start(void) {
    if (!mic_q) {
        AudioStreamBasicDescription f = pcm16(1);
        if (AudioQueueNewInput(&f, mic_cb, NULL, NULL, NULL, 0, &mic_q)) { mic_q = NULL; return false; }
        for (int i = 0; i < 3; i++) {
            AudioQueueBufferRef b;
            if (!AudioQueueAllocateBuffer(mic_q, 512 * 2, &b)) AudioQueueEnqueueBuffer(mic_q, b, 0, NULL);
        }
    }
    mic_rp_ok = false;
    return AudioQueueStart(mic_q, NULL) == noErr;
}

void plat_mic_stop(void) { if (mic_q) AudioQueueStop(mic_q, true); }

float plat_mic_sample(void) {
    double w = (double)atomic_load(&mic_written);
    double dist = w - mic_rp;
    if (!mic_rp_ok || dist < 32 || dist > 4000) {      // (re)start ~25 ms behind the writer
        if (w < 800) return 0;
        mic_rp = w - 800;
        mic_rp_ok = true;
    }
    u64 i0 = (u64)mic_rp;
    float fr = (float)(mic_rp - (double)i0);
    float a = mic_ring[i0 % MIC_RING], b = mic_ring[(i0 + 1) % MIC_RING];
    mic_rp += 1.0;                                     // both run at 32 kHz
    return (a + (b - a) * fr) * (1.0f / 32768.0f);
}

// ---------------------------------------------------------------- the window ---

@interface GameView : NSView
@end

@implementation GameView {
    int drag_mode;                       // 0 none, 1 stylus (bottom screen), 2 turn the view (top)
    NSPoint last;
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (BOOL)isFlipped { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }

- (void)updatePixelWidth {
    CGFloat s = self.window ? self.window.backingScaleFactor : 2.0;
    atomic_store(&view_px_w, (int)lround(self.bounds.size.width * s));
}
- (void)setFrameSize:(NSSize)sz { [super setFrameSize:sz]; [self updatePixelWidth]; }
- (void)viewDidChangeBackingProperties { [self updatePixelWidth]; }

- (NSPoint)logical:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    return NSMakePoint(p.x * CANVAS_W / self.bounds.size.width, p.y * CANVAS_H / self.bounds.size.height);
}

static void set_stylus(NSPoint p) {
    in_tx = (int)fmax(0, fmin(319, p.x - BOT_X));
    in_ty = (int)fmax(0, fmin(239, p.y - BOT_Y));
}

- (void)mouseDown:(NSEvent *)e {
    NSPoint p = [self logical:e];
    os_unfair_lock_lock(&in_lock);
    if (p.y >= BOT_Y) {
        drag_mode = 1;
        set_stylus(p);
        in_mouse_down = true;
        in_click_pending = true;
        in_release_next = false;
    } else {
        drag_mode = 2;
    }
    os_unfair_lock_unlock(&in_lock);
    last = p;
}

- (void)mouseDragged:(NSEvent *)e {
    NSPoint p = [self logical:e];
    os_unfair_lock_lock(&in_lock);
    if (drag_mode == 1) set_stylus(p);
    else if (drag_mode == 2) { in_drag_dx += p.x - last.x; in_drag_dy += p.y - last.y; }
    os_unfair_lock_unlock(&in_lock);
    last = p;
}

- (void)mouseUp:(NSEvent *)e {
    NSPoint p = [self logical:e];
    os_unfair_lock_lock(&in_lock);
    if (drag_mode == 1) { set_stylus(p); in_mouse_down = false; }
    os_unfair_lock_unlock(&in_lock);
    drag_mode = 0;
}

static u32 key_bit(NSEvent *e) {
    switch (e.keyCode) {
    case 123: return KEY_DLEFT;
    case 124: return KEY_DRIGHT;
    case 125: return KEY_DDOWN;
    case 126: return KEY_DUP;
    case 49:  return KEY_A;              // space
    case 51:  return KEY_B;              // delete
    case 48:  return KEY_SELECT;         // tab
    }
    NSString *c = e.charactersIgnoringModifiers.lowercaseString;
    if (c.length != 1) return 0;
    switch ([c characterAtIndex:0]) {
    case 'a': return KEY_A;
    case 'b': return KEY_B;
    case 'x': return KEY_X;
    case 'y': return KEY_Y;
    case 'q': return KEY_L;
    case 'w': return KEY_R;
    case '1': return KEY_ZL;
    case '2': return KEY_ZR;
    }
    return 0;
}

- (void)keyDown:(NSEvent *)e {
    if (e.modifierFlags & NSEventModifierFlagCommand) { [super keyDown:e]; return; }
    u32 b = key_bit(e);
    if (!b) return;
    os_unfair_lock_lock(&in_lock);
    in_keys |= b;
    os_unfair_lock_unlock(&in_lock);
}

- (void)keyUp:(NSEvent *)e {
    u32 b = key_bit(e);
    os_unfair_lock_lock(&in_lock);
    in_keys &= ~b;
    os_unfair_lock_unlock(&in_lock);
}

- (void)releaseAll {
    os_unfair_lock_lock(&in_lock);
    in_keys = 0;
    if (in_mouse_down) { in_mouse_down = false; in_release_next = true; }
    os_unfair_lock_unlock(&in_lock);
    drag_mode = 0;
}
@end

// ------------------------------------------------------------- application ---

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(strong) NSWindow *window;
@end

static pthread_t app_thread;
static atomic_bool app_done;

static void *app_main(void *arg) {
    (void)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    nodetrix_main();
    atomic_store(&app_done, true);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (atomic_load(&quit_requested)) [NSApp replyToApplicationShouldTerminate:YES];
        else [NSApp terminate:nil];
    });
    return NULL;
}

@implementation AppDelegate

- (void)buildMenu {
    NSMenu *bar = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new];
    [bar addItem:appItem];
    NSMenu *m = [NSMenu new];
    [m addItemWithTitle:@"About nodeTrix" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [m addItem:NSMenuItem.separatorItem];
    [m addItemWithTitle:@"Open nodeTrix Folder (samples, slots)" action:@selector(openFolder:) keyEquivalent:@"o"];
    [m addItemWithTitle:@"Keyboard Controls" action:@selector(showKeys:) keyEquivalent:@"/"];
    [m addItem:NSMenuItem.separatorItem];
    [m addItemWithTitle:@"Hide nodeTrix" action:@selector(hide:) keyEquivalent:@"h"];
    [m addItemWithTitle:@"Quit nodeTrix" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = m;
    NSMenuItem *winItem = [NSMenuItem new];
    [bar addItem:winItem];
    NSMenu *w = [[NSMenu alloc] initWithTitle:@"Window"];
    [w addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [w addItemWithTitle:@"Actual Size (2x)" action:@selector(sizeTwo:) keyEquivalent:@"0"];
    winItem.submenu = w;
    NSApp.mainMenu = bar;
    NSApp.windowsMenu = w;
}

- (void)openFolder:(id)s {
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:@(app_dir)]];
}

- (void)showKeys:(id)s {
    NSAlert *a = [NSAlert new];
    a.messageText = @"nodeTrix keyboard controls";
    a.informativeText =
        @"Mouse on the bottom screen: the stylus\n"
        @"Drag on the top screen: turn the view (circle pad)\n\n"
        @"Space or A: play / stop (A button)\n"
        @"B or Delete: back to the start of the bar\n"
        @"Arrow keys: D-pad\n"
        @"Q / W: L / R (track select, SYNTH zoom)\n"
        @"1 / 2: ZL / ZR (expand; ZR held = x10)\n"
        @"X: mute track    Y: reset the view\n"
        @"Tab: settings (SELECT)\n\n"
        @"Put .wav files for SAMPLE nodes in the nodeTrix folder's \"samples\" folder "
        @"(nodeTrix menu > Open nodeTrix Folder), then restart.";
    [a runModal];
    [(GameView *)game_view releaseAll];
}

- (void)sizeTwo:(id)s {
    NSRect f = self.window.frame;
    NSRect c = [self.window contentRectForFrameRect:f];
    c.origin.y += c.size.height - CANVAS_H * 2;
    c.size = NSMakeSize(CANVAS_W * 2, CANVAS_H * 2);
    [self.window setFrame:[self.window frameRectForContentRect:c] display:YES animate:YES];
}

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    [self buildMenu];
    CGFloat sc = 1.5;
    NSRect vis = NSScreen.mainScreen.visibleFrame;
    if (CANVAS_H * sc > vis.size.height - 40) sc = (vis.size.height - 40) / CANVAS_H;
    NSRect r = NSMakeRect(0, 0, CANVAS_W * sc, CANVAS_H * sc);
    self.window = [[NSWindow alloc] initWithContentRect:r
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"nodeTrix";
    self.window.delegate = self;
    self.window.contentAspectRatio = NSMakeSize(CANVAS_W, CANVAS_H);
    self.window.contentMinSize = NSMakeSize(CANVAS_W, CANVAS_H);
    self.window.backgroundColor = [NSColor colorWithCalibratedRed:0.02 green:0.025 blue:0.035 alpha:1];
    self.window.frameAutosaveName = @"nodeTrix";
    GameView *v = [[GameView alloc] initWithFrame:r];
    v.wantsLayer = YES;
    v.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
    v.layer.contentsGravity = kCAGravityResize;
    v.layer.magnificationFilter = kCAFilterLinear;
    game_view = v;
    self.window.contentView = v;
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:v];
    [v updatePixelWidth];
    [NSApp activateIgnoringOtherApps:YES];

    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8 * 1024 * 1024);
    pthread_create(&app_thread, &a, app_main, NULL);
    pthread_attr_destroy(&a);
}

- (void)windowDidResignKey:(NSNotification *)n { [(GameView *)game_view releaseAll]; }
- (void)windowDidChangeBackingProperties:(NSNotification *)n { [(GameView *)game_view updatePixelWidth]; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { return YES; }

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)a {
    if (atomic_load(&app_done)) return NSTerminateNow;
    atomic_store(&quit_requested, true);   // main.c leaves its loop, stops audio and saves
    return NSTerminateLater;
}
@end

int main(int argc, const char **argv) {
    (void)argc; (void)argv;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    ticks_per_mach = (double)tb.numer / tb.denom * SYSCLOCK_ARM11 / 1e9;
    mach_per_ns = (double)tb.denom / tb.numer;
    selftest_dir = getenv("NT_SELFTEST");
    if (selftest_dir) {                              // no window, no sound; files in a scratch folder
        snprintf(app_dir, sizeof app_dir, "%s/appdata", selftest_dir);
        mkdirs(app_dir);
        nodetrix_main();
        printf("self-test %s\n", selftest_fail ? "FAILED" : "passed");
        return selftest_fail ? 1 : 0;
    }
    paths_init();
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate *d = [AppDelegate new];
        app.delegate = d;
        [app run];
    }
    return 0;
}
