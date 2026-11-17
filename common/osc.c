#include "osc.h"
#include <stdarg.h>
#include <string.h>

static size_t pad4(size_t n) { return (n + 3) & ~(size_t)3; }

// Write a NUL-terminated string padded to a 4-byte boundary.
static int put_str(uint8_t *buf, size_t cap, size_t *pos, const char *s) {
    size_t len = strlen(s) + 1;
    size_t padded = pad4(len);
    if (*pos + padded > cap) return 0;
    memcpy(buf + *pos, s, len);
    memset(buf + *pos + len, 0, padded - len);
    *pos += padded;
    return 1;
}

static int put_u32(uint8_t *buf, size_t cap, size_t *pos, uint32_t v) {
    if (*pos + 4 > cap) return 0;
    buf[(*pos)++] = (uint8_t)(v >> 24);
    buf[(*pos)++] = (uint8_t)(v >> 16);
    buf[(*pos)++] = (uint8_t)(v >> 8);
    buf[(*pos)++] = (uint8_t)(v);
    return 1;
}

size_t osc_encode(uint8_t *buf, size_t cap, const char *address, const char *types, ...) {
    va_list ap;
    va_start(ap, types);
    size_t n = osc_vencode(buf, cap, address, types, ap);
    va_end(ap);
    return n;
}

size_t osc_vencode(uint8_t *buf, size_t cap, const char *address, const char *types, va_list ap) {
    size_t pos = 0;
    char tags[32];
    size_t nt = strlen(types);
    if (nt + 2 > sizeof(tags)) return 0;
    tags[0] = ',';
    memcpy(tags + 1, types, nt + 1);

    if (!put_str(buf, cap, &pos, address)) return 0;
    if (!put_str(buf, cap, &pos, tags)) return 0;

    for (size_t i = 0; i < nt; i++) {
        int ok = 0;
        switch (types[i]) {
        case 'f': {
            float f = (float)va_arg(ap, double);
            uint32_t u;
            memcpy(&u, &f, 4);
            ok = put_u32(buf, cap, &pos, u);
            break;
        }
        case 'i':
            ok = put_u32(buf, cap, &pos, (uint32_t)va_arg(ap, int));
            break;
        case 's':
            ok = put_str(buf, cap, &pos, va_arg(ap, const char *));
            break;
        }
        if (!ok) return 0;
    }
    return pos;
}
