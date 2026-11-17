// A sequencer pattern: BPM + the five shape tracks. Saved in slots and
// strung together by SONG mode.
#pragma once
#include <stdbool.h>

#define PAT_NTRACKS 5

typedef struct {
    int  divs, pulses, rot;
    bool mute;
} PatTrack;

typedef struct {
    int      bpm;
    PatTrack tr[PAT_NTRACKS];
} Pattern;

// Implemented in main.c. Both take the sequencer lock.
void seq_get_pattern(Pattern *out);
void seq_set_pattern(const Pattern *p);
