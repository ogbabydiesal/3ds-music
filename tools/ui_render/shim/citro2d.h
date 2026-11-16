// Minimal stand-in for citro2d: same signatures, implemented with CoreGraphics in render.c.
#pragma once
#include "3ds.h"

typedef void *C2D_TextBuf;
typedef void *C2D_Font;

static inline u32 C2D_Color32(u8 r, u8 g, u8 b, u8 a) {
    return r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24);
}

enum { C2D_WithColor = BIT(1), C2D_AlignLeft = 0, C2D_AlignRight = 1 << 2, C2D_AlignCenter = 2 << 2 };

bool C2D_DrawRectSolid(float x, float y, float z, float w, float h, u32 clr);
bool C2D_DrawLine(float x0, float y0, u32 clr0, float x1, float y1, u32 clr1, float thickness, float depth);
bool C2D_DrawCircleSolid(float x, float y, float z, float radius, u32 clr);
