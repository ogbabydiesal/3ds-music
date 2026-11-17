#include "t.h"
#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int16_t t_buf[SR * T_SECONDS * 2];
static int fails = 0, checks = 0;

// ----------------------------------------------------- stubs for UI code ---
// store.c and modseq.c also hold their pages' drawing code; it isn't run here.
bool C2D_DrawRectSolid(float x, float y, float z, float w, float h, u32 c) { return true; }
bool C2D_DrawLine(float a, float b, u32 c, float d, float e, u32 f, float g, float h) { return true; }
bool C2D_DrawCircleSolid(float x, float y, float z, float r, u32 c) { return true; }
void text(float x, float y, float s, u32 c, u32 a, const char *f, ...) {}
void rect_outline(float x, float y, float w, float h, u32 c) {}
u32  track_color(int t, float f, bool m) { return 0; }
bool keyboard_number(const char *h, float i, float *o) { return false; }
const char *track_names[5];
const u8 track_rgb[5][3];
static Pattern cur_pat = { 120 };
void seq_get_pattern(Pattern *o) { *o = cur_pat; }
void seq_set_pattern(const Pattern *p) { cur_pat = *p; }
bool seq_is_playing(void) { return false; }

// ----------------------------------------------------------------- report ---

void t_section(const char *name) { printf("\n%s\n", name); }

void t_check(bool ok, const char *fmt, ...) {
    checks++;
    if (!ok) fails++;
    printf("  %s  ", ok ? "ok  " : "FAIL");
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

// ---------------------------------------------------------------- helpers ---

void t_clear_patch(void) {
    for (int i = 0; i < SYN_MAX_NODES; i++)
        if (syn_nodes[i].used) syn_delete_node(i);
}

void t_render(int frames) {
    memset(t_buf, 0, sizeof t_buf);
    for (int done = 0; done < frames; ) {           // in 256-frame blocks, like the 3DS
        int n = frames - done < 256 ? frames - done : 256;
        syn_render(t_buf + 2 * done, n);
        done += n;
    }
}

float t_sample(int ch, int i) { return t_buf[2 * i + ch] / (32000.0f * 0.9f); }

double t_rms(int ch, int a, int b) {
    double s = 0;
    for (int i = a; i < b; i++) { double x = t_sample(ch, i); s += x * x; }
    return sqrt(s / (b - a));
}

double t_amp_at(double hz, int a, int b) {
    double c = 0, s = 0;
    for (int i = a; i < b; i++) {
        double x = t_sample(0, i);
        c += x * cos(2 * M_PI * hz * i / SR);
        s += x * sin(2 * M_PI * hz * i / SR);
    }
    return 2 * sqrt(c * c + s * s) / (b - a);
}

double t_zc_freq(int a, int b) {
    int z = 0;
    for (int i = a + 1; i < b; i++) if ((t_buf[2 * (i - 1)] < 0) != (t_buf[2 * i] < 0)) z++;
    return z / 2.0 / ((b - a) / (double)SR);
}

double t_lr_corr(int a, int b) {
    double lr = 0, ll = 0, rr = 0;
    for (int i = a; i < b; i++) {
        double l = t_buf[2 * i], r = t_buf[2 * i + 1];
        lr += l * r; ll += l * l; rr += r * r;
    }
    return lr / sqrt(ll * rr + 1e-9);
}

double t_clipped(int a, int b) {
    int n = 0;
    for (int i = 2 * a; i < 2 * b; i++) n += abs(t_buf[i]) >= 32000;
    return n / (2.0 * (b - a));
}

double t_db(double x) { return 20 * log10(x + 1e-12); }

static void w16(FILE *f, int v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void w32(FILE *f, uint32_t v) { w16(f, v & 0xFFFF); w16(f, v >> 16); }

void t_write_wav(const char *path, int fmt, int chans, int rate, int bits, int frames,
                 double (*fn)(int i, int c, int rate)) {
    FILE *f = fopen(path, "wb");
    if (!f) { t_check(false, "can't write %s", path); return; }
    int bps = bits / 8, data = frames * chans * bps, fmt_len = fmt == 0xFFFE ? 40 : 16;
    fwrite("RIFF", 1, 4, f); w32(f, 4 + 14 + 8 + fmt_len + 8 + data); fwrite("WAVE", 1, 4, f);
    fwrite("LIST", 1, 4, f); w32(f, 5); fwrite("junk\0\0", 1, 6, f);   // odd-sized chunk + pad byte to skip
    fwrite("fmt ", 1, 4, f); w32(f, fmt_len);
    w16(f, fmt); w16(f, chans); w32(f, rate); w32(f, rate * chans * bps); w16(f, chans * bps); w16(f, bits);
    if (fmt == 0xFFFE) {                                                  // cbSize, valid bits, mask, GUID
        w16(f, 22); w16(f, bits); w32(f, 0);
        w16(f, 1); for (int k = 0; k < 14; k++) fputc(0, f);
    }
    fwrite("data", 1, 4, f); w32(f, data);
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < chans; c++) {
            double v = fn(i, c, rate);
            if (fmt == 3) { float x = (float)v; fwrite(&x, 4, 1, f); continue; }
            long q = lround(v * ((1L << (bits - 1)) - 1));
            if (bits == 8) fputc((int)(q + 128), f);
            else for (int k = 0; k < bps; k++) fputc((int)((q >> (8 * k)) & 255), f);
        }
    fclose(f);
}

// ------------------------------------------------------------------- main ---

int main(void) {
    // Everything runs in the current directory: store/ (slots), old/ (pre-rename
    // slots) and wavs/ are created fresh by the Makefile.
    suite_dsp();             // also loads the test WAVs (samples are loaded once, like at app start)
    suite_graph();
    suite_mod();
    suite_store();
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
