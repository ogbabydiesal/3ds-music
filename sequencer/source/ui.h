// Shared drawing helpers / constants between the sequencer and synth pages.
#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdbool.h>

#define TOP_W 400
#define BOT_W 320

#define COL_FG      C2D_Color32(235, 240, 245, 255)
#define COL_DIM     C2D_Color32(140, 145, 160, 255)
#define COL_PANEL   C2D_Color32(30, 37, 44, 255)
#define COL_PANEL2  C2D_Color32(44, 56, 66, 255)
#define COL_BTN     C2D_Color32(58, 69, 80, 255)
#define COL_LINE    C2D_Color32(90, 98, 110, 255)
#define COL_ACCENT  C2D_Color32(52, 195, 160, 255)
#define COL_BG_BOT  C2D_Color32(16, 20, 24, 255)

extern C2D_TextBuf tbuf;
extern C2D_Font    ui_font;           // Arial from romfs (NULL = system font)

extern const char *track_names[5];
extern const u8    track_rgb[5][3];
u32  track_color(int t, float flash, bool muted);

void text(float x, float y, float scale, u32 col, u32 align, const char *fmt, ...);
void rect_outline(float x, float y, float w, float h, u32 col);

// Settings gear: 8 square teeth around a ring.
static inline void draw_gear(float cx, float cy, u32 col, u32 bg) {
    for (int k = 0; k < 8; k++) {
        float a = k * 0.785398f, c = cosf(a), s = sinf(a);
        C2D_DrawLine(cx + c * 4.5f, cy + s * 4.5f, col, cx + c * 8.0f, cy + s * 8.0f, col, 3.0f, 0.5f);
    }
    C2D_DrawCircleSolid(cx, cy, 0.5f, 6.0f, col);
    C2D_DrawCircleSolid(cx, cy, 0.5f, 2.6f, bg);
}
