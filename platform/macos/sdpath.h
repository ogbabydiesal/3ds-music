// Force-included into every nodeTrix source on the Mac (-include sdpath.h).
// The app reads and writes "sdmc:/3ds/..." paths; these wrappers send them to
//   sdmc:/3ds/nodeTrix/...  ->  ~/Documents/nodeTrix/...   (samples, slots, settings)
//   any other sdmc:/ path   ->  ~/Documents/nodeTrix/.sd/...
// System headers are included first so their declarations keep the real names.
#pragma once
#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

const char *nt_path(const char *p);         // rewritten path (thread-local buffer, 4 deep)

static inline FILE *nt_fopen(const char *p, const char *m) { return fopen(nt_path(p), m); }
static inline int   nt_mkdir(const char *p, mode_t m)      { return mkdir(nt_path(p), m); }
static inline int   nt_stat(const char *p, struct stat *s) { return stat(nt_path(p), s); }
static inline DIR  *nt_opendir(const char *p)               { return opendir(nt_path(p)); }
static inline int   nt_remove(const char *p)                { return remove(nt_path(p)); }
static inline int   nt_rename(const char *a, const char *b) {
    char from[1024];
    snprintf(from, sizeof from, "%s", nt_path(a));
    return rename(from, nt_path(b));
}

#define fopen   nt_fopen
#define mkdir   nt_mkdir
#define stat(p, s) nt_stat(p, s)
#define opendir nt_opendir
#define remove  nt_remove
#define rename  nt_rename
