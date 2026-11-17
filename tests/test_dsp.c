// Sound: every node's DSP against what it should measurably do.
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static double sine440(int i, int c, int rate) { return 0.5 * sin(2 * M_PI * 440 * i / rate); }
static double ramp(int i, int c, int rate) { return c ? -0.25 : 0.25; }        // L/R differ: mono mix = 0
static double quarter(int i, int c, int rate) { return 0.25; }

// Gain in dB of a sine at hz through a FILTER (at a level that leaves room for resonance)
static double filter_gain_at(int type, float fc, float q, float hz, float level) {
    t_clear_patch();
    int o = syn_add_node(NODE_OSC, 0, 0), f = syn_add_node(NODE_FILTER, 0, 0);
    syn_set_param(o, OSC_FREQ, hz);
    syn_set_param(o, OSC_LEVEL, level);
    syn_connect(o, f);
    syn_toggle_output(f, 0);
    syn_set_param(f, FLT_TYPE, type);
    syn_set_param(f, FLT_CUTOFF, fc);
    syn_set_param(f, FLT_Q, q);
    t_render(SR / 2);
    return t_db(t_amp_at(hz, SR / 4, SR / 2) / level);
}
static double filter_gain(int type, float fc, float q, float hz) { return filter_gain_at(type, fc, q, hz, 0.5f); }

// Lowest and highest frequency of L over frames a..b, period by period
// (rising zero crossings, interpolated between samples).
static void freq_range(int a, int b, double *lo, double *hi) {
    double last = -1;
    *lo = 1e9; *hi = 0;
    for (int i = a + 1; i < b; i++) {
        float x0 = t_sample(0, i - 1), x1 = t_sample(0, i);
        if (x0 >= 0 || x1 < 0) continue;
        double t = i - 1 + x0 / (x0 - x1);
        if (last >= 0) {
            double f = SR / (t - last);
            if (f < *lo) *lo = f;
            if (f > *hi) *hi = f;
        }
        last = t;
    }
}

// Which of the candidate frequencies is loudest in L over frames a..b
static double loudest(const double *hz, int n, int a, int b) {
    double best = 0, amp = -1;
    for (int k = 0; k < n; k++) {
        double v = t_amp_at(hz[k], a, b);
        if (v > amp) { amp = v; best = hz[k]; }
    }
    return best;
}

static void wavs(void) {
    t_section("WAV loader");
    mkdir("wavs", 0777);
    t_write_wav("wavs/Tone 440.wav", 1, 2, 44100, 16, 44100, sine440);   // 1 s stereo; space -> _
    t_write_wav("wavs/b_8bit.wav", 1, 1, 22050, 8, 2205, quarter);
    t_write_wav("wavs/c_24bit.wav", 1, 1, 48000, 24, 4800, quarter);
    t_write_wav("wavs/d_float.wav", 3, 1, 32000, 32, 3200, quarter);
    t_write_wav("wavs/e_ext24.wav", 0xFFFE, 2, 96000, 24, 9600, ramp);
    t_write_wav("wavs/f_32bit.wav", 1, 1, 32000, 32, 320, quarter);
    FILE *f = fopen("wavs/g_broken.wav", "wb");
    fputs("RIFF....WAVEnot really", f);
    fclose(f);
    f = fopen("wavs/notes.txt", "w");
    fputs("not audio", f);
    fclose(f);

    int n = samples_init("wavs");
    CHECK(n == 6, "6 good files load; the broken WAV and the .txt are skipped (%d loaded)", n);
    const char *names[] = { "b_8bit", "c_24bit", "d_float", "e_ext24", "f_32bit", "Tone_440" };
    bool sorted = true;
    for (int i = 0; i < 6 && i < n; i++) sorted &= !strcmp(samples_get(i)->name, names[i]);
    CHECK(sorted, "sorted by name, ignoring case; spaces become _ (first: %s, last: %s)",
          n ? samples_get(0)->name : "-", n ? samples_get(n - 1)->name : "-");
    const struct { const char *name; int len, rate; float v; } want[] = {
        { "b_8bit", 2205, 22050, 0.25f }, { "c_24bit", 4800, 48000, 0.25f }, { "d_float", 3200, 32000, 0.25f },
        { "e_ext24", 9600, 96000, 0.0f }, { "f_32bit", 320, 32000, 0.25f },
    };
    for (int k = 0; k < 5; k++) {
        int i = samples_find(want[k].name);
        const Sample *s = samples_get(i);
        float v = s ? s->data[10] / 32767.0f : -9;
        CHECK(s && s->len == want[k].len && s->rate == want[k].rate && fabsf(v - want[k].v) < 0.01f,
              "%-8s %d frames @ %d Hz, value %.3f (want %.2f)", want[k].name, s ? s->len : 0, s ? s->rate : 0, v, want[k].v);
    }
}

static void filter(void) {
    t_section("FILTER (state-variable, cutoff 1 kHz)");
    double lp_pass = filter_gain(FT_LP, 1000, 0.707f, 100), lp_fc = filter_gain(FT_LP, 1000, 0.707f, 1000),
           lp_stop = filter_gain(FT_LP, 1000, 0.707f, 8000);
    CHECK(fabs(lp_pass) < 0.5 && fabs(lp_fc + 3) < 0.7 && lp_stop < -36,
          "LP: 100 Hz %.1f dB, 1 kHz %.1f dB (-3), 8 kHz %.1f dB", lp_pass, lp_fc, lp_stop);
    double hp_stop = filter_gain(FT_HP, 1000, 0.707f, 100), hp_fc = filter_gain(FT_HP, 1000, 0.707f, 1000),
           hp_pass = filter_gain(FT_HP, 1000, 0.707f, 8000);
    CHECK(hp_stop < -36 && fabs(hp_fc + 3) < 0.7 && fabs(hp_pass) < 0.5,
          "HP: 100 Hz %.1f dB, 1 kHz %.1f dB (-3), 8 kHz %.1f dB", hp_stop, hp_fc, hp_pass);
    double bp_fc = filter_gain(FT_BP, 1000, 2, 1000), bp_off = filter_gain(FT_BP, 1000, 2, 4000);
    CHECK(fabs(bp_fc) < 0.5 && bp_off < -10, "BP Q 2: 1 kHz %.1f dB (0), 4 kHz %.1f dB", bp_fc, bp_off);
    double n_fc = filter_gain(FT_NOTCH, 1000, 0.707f, 1000), n_off = filter_gain(FT_NOTCH, 1000, 0.707f, 8000);
    CHECK(n_fc < -40 && fabs(n_off) < 0.5, "NOTCH: 1 kHz %.1f dB (removed), 8 kHz %.1f dB", n_fc, n_off);
    double res = filter_gain_at(FT_LP, 1000, 10, 1000, 0.05f);
    CHECK(fabs(res - 20) < 1, "LP Q 10 resonates at the cutoff: %+.1f dB (Q 10 = +20 dB)", res);
}

static void delay(void) {
    t_section("DELAY");
    t_clear_patch();
    int e = syn_add_node(NODE_ENV, 0, 0), d = syn_add_node(NODE_DELAY, 0, 0);
    syn_set_param(e, ENV_A, 0.5f); syn_set_param(e, ENV_D, 2); syn_set_param(e, ENV_LEN, 2); syn_set_param(e, ENV_R, 1);
    syn_connect(e, d);
    syn_toggle_output(d, 0);
    syn_set_param(d, DLY_TIME, 250); syn_set_param(d, DLY_FEEDBACK, 0.5f);
    syn_set_param(d, DLY_CUTOFF, 3000); syn_set_param(d, DLY_MIX, 1);
    t_render(SR / 10);                                  // let TIME settle
    syn_trigger(0);
    t_render(SR * 2);
    int at[5]; float pk[5];
    for (int w = 0; w < 5; w++) {
        pk[w] = 0; at[w] = 0;
        for (int i = w * SR / 4 + SR / 8; i < (w + 1) * SR / 4 + SR / 8; i++)
            if (fabsf(t_sample(0, i)) > pk[w]) { pk[w] = fabsf(t_sample(0, i)); at[w] = i; }
    }
    bool timing = true, halving = true;
    for (int w = 1; w < 5; w++) {
        timing &= abs(at[w] - at[w - 1] - SR / 4) <= SR / 500;      // within 2 ms
        if (w >= 2) halving &= pk[w] / pk[w - 1] > 0.3f && pk[w] / pk[w - 1] < 0.6f;
    }
    CHECK(timing, "echoes 250 ms apart: at %d, %d, %d, %d ms", at[1] * 1000 / SR, at[2] * 1000 / SR, at[3] * 1000 / SR, at[4] * 1000 / SR);
    CHECK(halving, "FEEDBACK 50%% roughly halves each echo: %.3f %.3f %.3f %.3f", pk[1], pk[2], pk[3], pk[4]);

    syn_set_param(d, DLY_FEEDBACK, 0.95f); syn_set_param(d, DLY_FTYPE, FT_BP); syn_set_param(d, DLY_Q, 20);
    for (int k = 0; k < 8; k++) { syn_trigger(0); t_render(SR); }
    CHECK(isfinite(syn_nodes[d].out) && t_clipped(0, SR) < 0.01,
          "95%% feedback through a Q 20 band-pass stays bounded (%.2f%% of samples at full scale)", 100 * t_clipped(0, SR));
}

static void chorus(void) {
    t_section("CHORUS");
    t_clear_patch();
    int o = syn_add_node(NODE_OSC, 0, 0), c = syn_add_node(NODE_CHORUS, 0, 0);
    syn_set_param(o, OSC_FREQ, 440);
    syn_connect(o, c);
    syn_toggle_output(c, 0);
    syn_set_param(c, CHO_MIX, 1);
    syn_set_param(c, CHO_RATE, 2);
    // A swept delay shifts pitch by f * (rate of delay change): a triangle LFO
    // sweeping +-D ms at R Hz gives +-f * 4 * D * R / 1000.
    double lo[2], hi[2];
    const double want[2] = { 440 * 4 * 1.6 * 2 / 1000, 440 * 4 * 4.5 * 2 / 1000 };   // LOW +-1.6 ms, HIGH +-4.5 ms
    for (int dep = 0; dep < 2; dep++) {
        syn_set_param(c, CHO_DEPTH, dep);
        t_render(SR * 2);
        freq_range(SR / 2, 2 * SR, &lo[dep], &hi[dep]);
        CHECK(fabs((hi[dep] - lo[dep]) / 2 - want[dep]) < 0.25 * want[dep] && fabs((hi[dep] + lo[dep]) / 2 - 440) < 2,
              "%s depth: pitch swings %.1f..%.1f Hz (want 440 +-%.1f)", dep ? "HIGH" : "LOW ", lo[dep], hi[dep], want[dep]);
    }
}

static void reverb(void) {
    t_section("REVERB (JCRev)");
    // T60 sets the comb feedback; the damping in each comb (as in STK) makes the
    // bright parts of noise fade a bit faster, so allow some slack.
    double rate[2];
    for (int k = 0; k < 2; k++) {
        float t60 = k ? 3.0f : 1.0f;
        t_clear_patch();
        int n = syn_add_node(NODE_NOISE, 0, 0), e = syn_add_node(NODE_ENV, 0, 0), r = syn_add_node(NODE_REVERB, 0, 0);
        syn_set_param(e, ENV_D, 5); syn_set_param(e, ENV_LEN, 5);
        syn_connect(n, e); syn_connect(e, r);
        syn_toggle_output(r, 0);
        syn_set_param(r, REV_T60, t60);
        syn_set_param(r, REV_MIX, 1);
        syn_trigger(0);
        t_render((int)(SR * (0.4 + t60 / 2 + 0.2)));
        int a = SR * 3 / 10, w = SR / 10, b = a + (int)(SR * t60 / 2);
        double drop = t_db(t_rms(0, a, a + w)) - t_db(t_rms(0, b, b + w));
        double corr = t_lr_corr(a, a + SR / 2);
        rate[k] = drop / (t60 / 2);
        CHECK(fabs(drop - 30) < 8 && fabs(corr) < 0.5,
              "T60 %.0f s: drops %.1f dB over T60/2 (want ~30); L/R correlation %.2f (stereo)", t60, drop, corr);
    }
    CHECK(rate[0] / rate[1] > 2.2, "T60 3 s fades %.1fx slower than T60 1 s (want ~3)", rate[0] / rate[1]);
}

static void grains(void) {
    t_section("GRAINS");
    t_clear_patch();
    int o = syn_add_node(NODE_OSC, 0, 0), g = syn_add_node(NODE_GRAINS, 0, 0);
    syn_set_param(o, OSC_FREQ, 220); syn_set_param(o, OSC_LEVEL, 0.5f);
    syn_connect(o, g);
    syn_toggle_output(g, 0);
    syn_set_param(g, GR_MIX, 1); syn_set_param(g, GR_RANDOM, 0); syn_set_param(g, GR_SIZE, 200);
    syn_set_param(g, GR_DENSITY, -0.4f);
    // Overlapping grains blur zero crossings, so ask which candidate pitch is loudest.
    const double cands[] = { 110, 165, 220, 330, 440, 660, 880 };
    const int nc = sizeof cands / sizeof cands[0];
    t_render(SR * 2);
    double f0 = loudest(cands, nc, SR, 2 * SR);
    syn_set_param(g, GR_PITCH, 12);  t_render(SR * 2);
    double up = loudest(cands, nc, SR, 2 * SR);
    syn_set_param(g, GR_PITCH, -12); t_render(SR * 2);
    double down = loudest(cands, nc, SR, 2 * SR);
    CHECK(f0 == 220 && up == 440 && down == 110, "PITCH 0 / +12 / -12 from a 220 Hz source: loudest %.0f / %.0f / %.0f Hz", f0, up, down);
    syn_set_param(g, GR_PITCH, 0);
    syn_set_param(g, GR_FREEZE, 1);
    syn_set_param(o, OSC_FREQ, 880);
    t_render(SR * 2);
    double frozen = loudest(cands, nc, SR, 2 * SR);
    syn_set_param(g, GR_FREEZE, 0);
    t_render(SR * 2);
    double live = loudest(cands, nc, SR, 2 * SR);
    CHECK(frozen == 220 && live == 880,
          "FREEZE keeps the old audio (%.0f Hz) after the source moves to 880; unfrozen %.0f Hz", frozen, live);
    syn_set_param(g, GR_DENSITY, 0);
    t_render(SR * 2);
    double quiet = t_rms(0, SR, 2 * SR);
    syn_trigger(0);
    t_render(SR / 2);
    double seeded = t_rms(0, 0, SR / 5);
    CHECK(quiet < 0.001 && seeded > 0.05, "DENSITY 0: silent (%.4f) until a pulse seeds a grain (%.3f)", quiet, seeded);
    syn_set_param(g, GR_DENSITY, 1); syn_set_param(g, GR_RANDOM, 1); syn_set_param(g, GR_SIZE, 1500);
    syn_set_param(g, GR_FEEDBACK, 0.9f); syn_set_param(g, GR_DIR, 2);
    t_render(SR * 4);
    CHECK(isfinite(syn_nodes[g].out) && t_clipped(0, SR * 4) < 0.05 && fabs(t_lr_corr(SR, 4 * SR)) < 0.9,
          "dense random grains with 90%% feedback: bounded (%.1f%% clipped), stereo (L/R corr %.2f)",
          100 * t_clipped(0, SR * 4), t_lr_corr(SR, 4 * SR));
}

static void sample(void) {
    t_section("SAMPLE");
    t_clear_patch();
    int s = syn_add_node(NODE_SAMPLE, 0, 0);
    syn_toggle_output(s, 1);
    syn_set_param(s, SMP_FILE, samples_find("Tone_440"));
    syn_set_param(s, SMP_LEVEL, 1);
    for (int k = 0; k < 2; k++) {
        syn_set_param(s, SMP_PITCH, 12 * k);
        syn_trigger(1);
        t_render(SR * 2);
        int last = 0;
        for (int i = 0; i < SR * 2; i++) if (fabsf(t_sample(0, i)) > 0.01f) last = i;
        double len = last / (double)SR, hz = t_zc_freq(SR / 20, SR / 4);
        CHECK(fabs(len - 1.0 / (1 + k)) < 0.02 && fabs(hz - 440 * (1 + k)) < 10,
              "PITCH %+d: a 1 s 44.1 kHz file plays %.2f s at ~%.0f Hz", 12 * k, len, hz);
    }
    syn_set_param(s, SMP_PITCH, 0); syn_set_param(s, SMP_LOOP, 1);
    syn_set_param(s, SMP_START, 0.5f); syn_set_param(s, SMP_END, 0.6f);
    syn_trigger(1);
    t_render(SR * 2);
    CHECK(t_rms(0, SR * 19 / 10, SR * 2) > 0.1, "LOOP 50-60%% still playing after 1.9 s");

    int m = syn_add_node(NODE_MIC, 0, 0);
    syn_toggle_output(m, 2);
    t_render(1000);
    CHECK(syn_nodes[m].out == 0, "MIC renders silence when there's no microphone");
}

void suite_dsp(void) {
    syn_init();
    mod_init();
    wavs();
    filter();
    delay();
    chorus();
    reverb();
    grains();
    sample();
}
