// nodeTrix host tests: the real engine, step sequencers, WAV loader and slot
// files compiled for the computer (no 3DS needed). Run with `make -C tests`.
#pragma once
#include "modseq.h"
#include "samples.h"
#include "store.h"
#include "synth.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define SR SYN_SAMPLE_RATE
#define T_SECONDS 12
extern int16_t t_buf[SR * T_SECONDS * 2];        // interleaved L,R, as syn_render writes it

void t_section(const char *name);
void t_check(bool ok, const char *fmt, ...);
#define CHECK(cond, ...) t_check((cond), __VA_ARGS__)

void   t_clear_patch(void);                       // delete every node
void   t_render(int frames);                      // syn_render into t_buf (frames <= SR * T_SECONDS)
float  t_sample(int ch, int i);                   // engine value of a rendered sample (undoes the 0.9 * 32000 output scale)
double t_rms(int ch, int a, int b);               // engine units, frames a..b
double t_amp_at(double hz, int a, int b);         // amplitude of a sine at hz in L, frames a..b
double t_zc_freq(int a, int b);                   // frequency from zero crossings of L
double t_lr_corr(int a, int b);
double t_clipped(int a, int b);                   // fraction of samples at full scale
double t_db(double x);

// Write a test WAV. fmt 1 = PCM, 3 = float, 0xFFFE = extensible PCM.
// Sample value for frame i, channel c comes from fn (-1..1).
void t_write_wav(const char *path, int fmt, int chans, int rate, int bits, int frames,
                 double (*fn)(int i, int c, int rate));

void suite_dsp(void);
void suite_graph(void);
void suite_mod(void);
void suite_store(void);
