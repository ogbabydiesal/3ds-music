// Mac stand-in for libctru's <3ds.h>: just the parts nodeTrix uses, implemented
// in mac_platform.m. The app's own code (sequencer/source, common) compiles
// against this unchanged.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os/lock.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef int32_t  Result;

#define BIT(n) (1U << (n))
#define R_FAILED(r)    ((Result)(r) < 0)
#define R_SUCCEEDED(r) ((Result)(r) >= 0)
#ifndef U64_MAX
#define U64_MAX UINT64_MAX
#endif

enum {
    KEY_A = BIT(0), KEY_B = BIT(1), KEY_SELECT = BIT(2), KEY_START = BIT(3),
    KEY_DRIGHT = BIT(4), KEY_DLEFT = BIT(5), KEY_DUP = BIT(6), KEY_DDOWN = BIT(7),
    KEY_R = BIT(8), KEY_L = BIT(9), KEY_X = BIT(10), KEY_Y = BIT(11),
    KEY_ZL = BIT(14), KEY_ZR = BIT(15), KEY_TOUCH = BIT(20),
};

// ---- time and threads ----
#define SYSCLOCK_ARM11 268111856ULL
#define CUR_THREAD_HANDLE 0
u64  svcGetSystemTick(void);
void svcSleepThread(s64 ns);
Result svcGetThreadPriority(s32 *out, u32 handle);

typedef struct NtThread *Thread;
typedef void (*ThreadFunc)(void *);
Thread threadCreate(ThreadFunc fn, void *arg, size_t stack, int prio, int core, bool detached);
Result threadJoin(Thread t, u64 timeout_ns);
void   threadFree(Thread t);

// A zero-initialised LightLock is valid on the 3DS, and so is an os_unfair_lock.
typedef os_unfair_lock LightLock;
static inline void LightLock_Init(LightLock *l)   { *l = OS_UNFAIR_LOCK_INIT; }
static inline void LightLock_Lock(LightLock *l)   { os_unfair_lock_lock(l); }
static inline void LightLock_Unlock(LightLock *l) { os_unfair_lock_unlock(l); }

// ---- system ----
bool aptMainLoop(void);
void gfxInitDefault(void);
void gfxSet3D(bool on);
void gfxExit(void);
float osGet3DSliderState(void);
Result romfsInit(void);
Result romfsExit(void);
Result socInit(u32 *buf, u32 size);
Result socExit(void);

// ---- input ----
typedef struct { u16 px, py; } touchPosition;
typedef struct { s16 dx, dy; } circlePosition;
void hidScanInput(void);
u32  hidKeysDown(void);
u32  hidKeysHeld(void);
u32  hidKeysUp(void);
u32  hidKeysDownRepeat(void);
void hidSetRepeatParameters(u32 delay, u32 interval);
void hidTouchRead(touchPosition *tp);
void hidCircleRead(circlePosition *cp);

typedef struct { s16 x, y, z; } accelVector;
typedef struct { s16 x, z, y; } angularRate;
Result HIDUSER_EnableAccelerometer(void);
Result HIDUSER_DisableAccelerometer(void);
Result HIDUSER_EnableGyroscope(void);
Result HIDUSER_DisableGyroscope(void);
void hidAccelRead(accelVector *v);
void hidGyroRead(angularRate *r);

// ---- software keyboard (a Mac text dialog) ----
typedef enum { SWKBD_TYPE_NORMAL, SWKBD_TYPE_QWERTY, SWKBD_TYPE_NUMPAD, SWKBD_TYPE_WESTERN } SwkbdType;
typedef enum { SWKBD_BUTTON_NONE = -1, SWKBD_BUTTON_LEFT = 0, SWKBD_BUTTON_MIDDLE, SWKBD_BUTTON_RIGHT,
               SWKBD_BUTTON_CONFIRM = SWKBD_BUTTON_RIGHT } SwkbdButton;
enum { SWKBD_ANYTHING = 0, SWKBD_NOTEMPTY, SWKBD_NOTEMPTY_NOTBLANK, SWKBD_NOTBLANK };
typedef struct {
    SwkbdType type;
    int  max_len;
    char hint[128];
    char initial[128];
} SwkbdState;
void swkbdInit(SwkbdState *kb, SwkbdType type, int nbuttons, int max_len);
void swkbdSetNumpadKeys(SwkbdState *kb, int left, int right);
void swkbdSetHintText(SwkbdState *kb, const char *hint);
void swkbdSetInitialText(SwkbdState *kb, const char *text);
void swkbdSetValidation(SwkbdState *kb, int valid, u32 filter, int max_digits);
SwkbdButton swkbdInputText(SwkbdState *kb, char *out, size_t outlen);

// net_local_ip() calls gethostid(); on the Mac it returns this machine's IPv4 address.
#define gethostid nt_gethostid
long nt_gethostid(void);
