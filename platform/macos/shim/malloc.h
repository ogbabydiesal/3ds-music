// macOS has no <malloc.h>; memalign is all nodeTrix needs from it.
#pragma once
#include <stdlib.h>
static inline void *memalign(size_t align, size_t size) {
    void *p = NULL;
    return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size) == 0 ? p : NULL;
}
