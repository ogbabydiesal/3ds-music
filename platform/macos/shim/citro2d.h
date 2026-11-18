// Mac stand-in for citro2d/citro3d: same calls, drawn with CoreGraphics in mac_platform.m.
// The two 3DS screens are drawn into one picture: top screen above, bottom screen below.
#pragma once
#include "3ds.h"

typedef struct NtTextBuf *C2D_TextBuf;
typedef struct NtFont    *C2D_Font;
typedef struct { const char *str; } C2D_Text;
typedef struct NtTarget C3D_RenderTarget;

typedef enum { GFX_TOP = 0, GFX_BOTTOM = 1 } gfxScreen_t;
typedef enum { GFX_LEFT = 0, GFX_RIGHT = 1 } gfx3dSide_t;

static inline u32 C2D_Color32(u8 r, u8 g, u8 b, u8 a) {
    return r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24);
}

enum { C2D_WithColor = BIT(1), C2D_AlignLeft = 0, C2D_AlignRight = 1 << 2, C2D_AlignCenter = 2 << 2 };

#define C3D_DEFAULT_CMDBUF_SIZE 0x40000
#define C2D_DEFAULT_MAX_OBJECTS 4096
#define C3D_FRAME_SYNCDRAW 1

bool C3D_Init(size_t cmdbuf);
void C3D_Fini(void);
bool C2D_Init(size_t max_objects);
void C2D_Fini(void);
void C2D_Prepare(void);
C3D_RenderTarget *C2D_CreateScreenTarget(gfxScreen_t screen, gfx3dSide_t side);
void C2D_TargetClear(C3D_RenderTarget *t, u32 color);
void C2D_SceneBegin(C3D_RenderTarget *t);
bool C3D_FrameBegin(u8 flags);
void C3D_FrameEnd(u8 flags);

C2D_TextBuf C2D_TextBufNew(size_t maxglyphs);
void C2D_TextBufClear(C2D_TextBuf b);
void C2D_TextBufDelete(C2D_TextBuf b);
C2D_Font C2D_FontLoad(const char *path);
void C2D_FontFree(C2D_Font f);
const char *C2D_TextFontParse(C2D_Text *t, C2D_Font f, C2D_TextBuf b, const char *str);
void C2D_TextOptimize(const C2D_Text *t);
// The colour is C2D_DrawText's optional last argument (with C2D_WithColor).
void nt_draw_text(const C2D_Text *t, u32 flags, float x, float y, float z, float sx, float sy, u32 color);
#define C2D_DrawText(t, flags, x, y, z, sx, sy, ...) nt_draw_text(t, flags, x, y, z, sx, sy, __VA_ARGS__ + 0)

bool C2D_DrawRectSolid(float x, float y, float z, float w, float h, u32 clr);
bool C2D_DrawLine(float x0, float y0, u32 clr0, float x1, float y1, u32 clr1, float thickness, float depth);
bool C2D_DrawCircleSolid(float x, float y, float z, float radius, u32 clr);
