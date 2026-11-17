// SAVE / LOAD / SONG.
//
// 8 slots on the SD card (sdmc:/3ds/nodeTrix/slots/slotN.txt), each holding a
// sequencer Pattern (BPM + 5 tracks) and the synth patch. SONG strings slots
// together: each step plays slot S for B bars, then moves on (looping). Song
// mode switches the sequencer pattern + BPM at bar lines; the synth patch
// currently loaded keeps playing.
#pragma once
#include <stdbool.h>
#include "pattern.h"

#define STORE_NSLOTS 8
#define SONG_MAX     16

enum { STORE_UI_SAVE, STORE_UI_LOAD, STORE_UI_SONG };

void store_init(void);                  // create folders, read slot summaries + song

bool store_save_slot(int slot);         // 0-based; current pattern + synth patch
bool store_load_slot(int slot);         // replaces pattern + synth patch
bool store_slot_used(int slot);

// UI popups
bool store_ui_is_open(void);
void store_ui_open(int which);
void store_ui_close(void);
void store_ui_touch_down(int x, int y);
void store_ui_draw(void);
void store_ui_tick(void);               // message timeouts

// Song playback. song_on_play / song_on_bar are called from the sequencer
// thread with the sequencer lock held; they never take that lock themselves.
bool song_mode_on(void);
bool song_on_play(Pattern *out);        // playback starting: step 1's pattern
bool song_on_bar(Pattern *out);         // a bar just ended: maybe the next step's pattern
void song_status(int *step, int *nsteps, int *bar, int *bars);

// Implemented in main.c
bool seq_is_playing(void);
