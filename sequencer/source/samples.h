// Sound files for the SAMPLE node: every .wav in sdmc:/3ds/nodeTrix/samples/
// is loaded once at startup (16-bit mono in memory, original sample rate).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SAMPLES_MAX        64
#define SAMPLES_NAME_LEN   40
#define SAMPLES_BUDGET     (24 * 1024 * 1024)   // bytes of audio kept in memory, all files together

typedef struct {
    char     name[SAMPLES_NAME_LEN];   // file name without ".wav"
    int16_t *data;                     // mono
    int      len;                      // frames
    int      rate;                     // Hz
} Sample;

// Scan `dir` (NULL = the SD card folder above) and load what fits. Returns the count.
int  samples_init(const char *dir);
int  samples_count(void);
const Sample *samples_get(int i);      // NULL if out of range
int  samples_find(const char *name);   // index or -1
const char *samples_dir(void);

// Exposed for tests: decode one WAV file (PCM 8/16/24/32-bit or float32, any channels).
bool samples_load_wav(const char *path, Sample *out);
