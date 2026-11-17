// Minimal stand-in for libctru's <3ds.h> so the SYNTH page UI compiles on macOS.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;

#define BIT(n) (1U << (n))

enum {
    KEY_A = BIT(0), KEY_B = BIT(1), KEY_SELECT = BIT(2), KEY_START = BIT(3),
    KEY_DRIGHT = BIT(4), KEY_DLEFT = BIT(5), KEY_DUP = BIT(6), KEY_DDOWN = BIT(7),
    KEY_R = BIT(8), KEY_L = BIT(9), KEY_X = BIT(10), KEY_Y = BIT(11),
};

typedef int16_t s16;
typedef int32_t Result;
typedef struct { s16 x, y, z; } accelVector;
typedef struct { s16 x, z, y; } angularRate;
Result HIDUSER_EnableAccelerometer(void);
Result HIDUSER_DisableAccelerometer(void);
Result HIDUSER_EnableGyroscope(void);
Result HIDUSER_DisableGyroscope(void);
void hidAccelRead(accelVector *v);
void hidGyroRead(angularRate *r);
