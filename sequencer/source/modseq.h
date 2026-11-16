// STEPS page: parameter step sequencers.
//
// Four lanes (A-D). Each lane is a row of 1..16 sliders. Every pulse of the
// lane's TRIG shape advances it to the next slider (looping after the last)
// and sets the TARGET synth parameter to that slider's value, scaled between
// MIN and MAX in the parameter's own units, just before the note plays.
#pragma once
#include <stdbool.h>

#define MOD_LANES      4
#define MOD_MAX_STEPS  16

typedef struct {
    int   node;                 // synth node index, -1 = no target
    int   param;                // parameter index within that node's class
    int   trig;                 // shape 0..4 whose pulses advance the lane
    int   nsteps;               // 1..MOD_MAX_STEPS
    float min_u, max_u;         // range, as 0..1 positions on the parameter's scale
    float v[MOD_MAX_STEPS];     // slider values 0..1
    volatile int cur;           // last played step, -1 = none yet
} ModLane;

extern ModLane mod_lanes[MOD_LANES];

void mod_init(void);
void mod_defaults(ModLane out[MOD_LANES]);   // fresh lanes (no target)
void mod_reset(void);               // playback (re)starts: next pulse plays step 1
void mod_on_pulse(int track);       // sequencer thread, before the synth is triggered
bool mod_lane_valid(const ModLane *l);
float mod_lane_value(const ModLane *l, int step);   // in the target parameter's units
void mod_set_all(const ModLane src[MOD_LANES]);     // loading a slot

// UI (page 3)
#define MOD_AREA_Y0 24
#define MOD_AREA_Y1 202
void mod_page_touch(bool down, bool held, bool up, int x, int y);
void mod_page_draw(void);
