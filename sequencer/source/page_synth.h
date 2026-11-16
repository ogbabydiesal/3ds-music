// SYNTH page: a carousel of node classes, a toolbar, and a zoomable patch
// canvas with box select / copy / paste, plus a popup for editing a node.
#pragma once
#include <3ds.h>
#include <stdbool.h>

#define SYNTH_AREA_Y0   24      // touches between these rows belong to the page
#define SYNTH_AREA_Y1   202

bool synth_page_popup_open(void);
// Called every frame while this page owns the touch. down/up are edge flags.
void synth_page_touch(bool down, bool held, bool up, int x, int y);
// Hardware buttons while the page is showing (L/R zoom, D-pad pan).
void synth_page_buttons(u32 kDown, u32 kHeld);
void synth_page_draw(void);         // canvas + carousel + toolbar (draw before header/footer)
void synth_page_draw_popup(void);   // drawn last, over everything on the bottom screen
