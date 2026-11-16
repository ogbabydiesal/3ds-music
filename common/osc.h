// Minimal OSC 1.0 message encoder (portable C, no allocations).
#pragma once
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

// Encode an OSC message into buf.
//   address: e.g. "/3ds/button/a"
//   types:   type tags without the leading comma, e.g. "ff", "i", "" (no args)
//            supported: 'f' (float, pass as double), 'i' (int32), 's' (string)
// Returns number of bytes written, or 0 if it didn't fit / bad type.
size_t osc_encode(uint8_t *buf, size_t cap, const char *address, const char *types, ...);

// Same as osc_encode, taking a va_list (for wrapping in other variadic functions).
size_t osc_vencode(uint8_t *buf, size_t cap, const char *address, const char *types, va_list ap);
