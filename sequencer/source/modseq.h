// STEPS page: 8 step sequencers (S1-S8).
//
// Each sequencer is a row of 1..16 sliders with a plain numeric MIN and MAX.
// Every pulse of its TRIG shape advances it to the next slider (looping after
// the last). Its value  MIN + (MAX - MIN) * slider  is written to every synth
// parameter assigned to it; assignments are made per parameter in the node's
// settings box on the SYNTH page. Values are clamped to each parameter's range.
// SLEW glides each new value in over that many milliseconds, along a LIN
// (straight) or LOG (fast, then easing in) curve; 0 ms jumps.
#pragma once
#include <stdbool.h>

#define MOD_SEQS       8
#define MOD_MAX_STEPS  16
#define MOD_MAX_SLEW   10000.0f   // ms

typedef struct {
    int   trig;                 // shape 0..4 whose pulses advance it
    int   nsteps;               // 1..MOD_MAX_STEPS
    float min, max;             // output range, plain numbers
    float v[MOD_MAX_STEPS];     // slider positions 0..1
    float slew_ms;              // 0..MOD_MAX_SLEW
    int   slew_curve;           // SLEW_LIN / SLEW_LOG (synth.h)
    volatile int cur;           // last played step, -1 = none yet
} ModSeq;

extern ModSeq mod_seqs[MOD_SEQS];

void  mod_defaults(ModSeq out[MOD_SEQS]);
void  mod_init(void);
void  mod_reset(void);              // playback (re)starts: next pulse plays step 1
void  mod_on_pulse(int track);      // sequencer thread, before the synth is triggered
float mod_seq_value(const ModSeq *m, int step);
void  mod_set_all(const ModSeq src[MOD_SEQS]);   // loading a slot
int   mod_seq_targets(int seq);     // how many synth parameters it drives

// UI (page 3)
#define MOD_AREA_Y0 24
#define MOD_AREA_Y1 202
void mod_page_touch(bool down, bool held, bool up, int x, int y);   // may open the keypad
void mod_page_draw(void);
