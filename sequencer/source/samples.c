#include "samples.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>                                     // strcasecmp
#include <sys/stat.h>

#define DEFAULT_DIR "sdmc:/3ds/nodeTrix/samples"

static Sample samples[SAMPLES_MAX];
static int    nsamples = 0;
static char   dir_used[256] = DEFAULT_DIR;

int samples_count(void) { return nsamples; }
const Sample *samples_get(int i) { return i >= 0 && i < nsamples ? &samples[i] : NULL; }
const char *samples_dir(void) { return dir_used; }

int samples_find(const char *name) {
    for (int i = 0; i < nsamples; i++)
        if (!strcmp(samples[i].name, name)) return i;
    return -1;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

// One sample (a single channel value) as -1..1.
static float decode(const uint8_t *p, int fmt, int bits) {
    if (fmt == 3 && bits == 32) { float f; memcpy(&f, p, 4); return f; }
    switch (bits) {
    case 8:  return (p[0] - 128) / 128.0f;
    case 16: return (int16_t)rd16(p) / 32768.0f;
    case 24: return (int32_t)((uint32_t)(p[0] << 8 | p[1] << 16 | (uint32_t)p[2] << 24)) / 2147483648.0f;
    case 32: return (int32_t)rd32(p) / 2147483648.0f;
    }
    return 0;
}

bool samples_load_wav(const char *path, Sample *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { fclose(f); return false; }

    int fmt = 0, chans = 0, rate = 0, bits = 0;
    uint8_t *raw = NULL;
    uint32_t raw_len = 0;
    uint8_t ck[8];
    while (fread(ck, 1, 8, f) == 8) {                      // walk the chunks
        uint32_t sz = rd32(ck + 4);
        if (!memcmp(ck, "fmt ", 4) && sz >= 16) {
            uint8_t b[40] = { 0 };
            uint32_t take = sz < sizeof b ? sz : sizeof b;
            if (fread(b, 1, take, f) != take) break;
            fmt = rd16(b); chans = rd16(b + 2); rate = (int)rd32(b + 4); bits = rd16(b + 14);
            if (fmt == 0xFFFE && sz >= 26) fmt = rd16(b + 24);   // WAVE_FORMAT_EXTENSIBLE: real format in the GUID
            if (sz > take) fseek(f, (long)(sz - take), SEEK_CUR);
        } else if (!memcmp(ck, "data", 4) && fmt) {
            raw = (uint8_t *)malloc(sz ? sz : 1);
            if (!raw) break;
            raw_len = (uint32_t)fread(raw, 1, sz, f);
            break;
        } else {
            fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);           // chunks are padded to even sizes
        }
    }
    fclose(f);

    bool ok = raw && (fmt == 1 || fmt == 3) && chans >= 1 && rate >= 1000 && rate <= 192000 &&
              (bits == 8 || bits == 16 || bits == 24 || bits == 32) && !(fmt == 3 && bits != 32);
    if (!ok) { free(raw); return false; }
    int bps = bits / 8, frame = bps * chans;
    int len = (int)(raw_len / (uint32_t)frame);
    if (len < 1) { free(raw); return false; }
    int16_t *data = (int16_t *)malloc((size_t)len * sizeof(int16_t));
    if (!data) { free(raw); return false; }
    for (int i = 0; i < len; i++) {                        // mix channels down to mono
        float s = 0;
        for (int c = 0; c < chans; c++) s += decode(raw + (size_t)i * frame + c * bps, fmt, bits);
        s /= chans;
        if (s > 1) s = 1;
        if (s < -1) s = -1;
        data[i] = (int16_t)(s * 32767.0f);
    }
    free(raw);
    out->data = data;
    out->len = len;
    out->rate = rate;
    return true;
}

static int cmp_name(const void *a, const void *b) {
    return strcasecmp(((const Sample *)a)->name, ((const Sample *)b)->name);
}

int samples_init(const char *dir) {
    if (dir) snprintf(dir_used, sizeof dir_used, "%s", dir);
    else {
        mkdir("sdmc:/3ds", 0777);
        mkdir("sdmc:/3ds/nodeTrix", 0777);
        mkdir(DEFAULT_DIR, 0777);
    }
    DIR *d = opendir(dir_used);
    if (!d) return 0;
    size_t used = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nsamples < SAMPLES_MAX) {
        const char *n = e->d_name;
        size_t l = strlen(n);
        if (l < 5 || n[0] == '.' || strcasecmp(n + l - 4, ".wav")) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir_used, n);
        struct stat st;
        if (stat(path, &st) == 0 && used + (size_t)st.st_size / 2 > SAMPLES_BUDGET) continue;   // rough fit check
        Sample s = { { 0 } };
        if (!samples_load_wav(path, &s)) continue;
        if (used + (size_t)s.len * 2 > SAMPLES_BUDGET) { free(s.data); continue; }
        used += (size_t)s.len * 2;
        size_t nl = l - 4 < SAMPLES_NAME_LEN - 1 ? l - 4 : SAMPLES_NAME_LEN - 1;
        memcpy(s.name, n, nl);
        s.name[nl] = 0;
        for (char *c = s.name; *c; c++) if (*c == ' ') *c = '_';   // names are saved as one word
        samples[nsamples++] = s;
    }
    closedir(d);
    qsort(samples, nsamples, sizeof samples[0], cmp_name);
    return nsamples;
}
