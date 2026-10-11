/* Minimal standalone host fixture shim. Production/Android use QEMU and GLib.
 * Only allocation and standard C declarations are needed by prim_rewrite.c. */
#ifndef PRIM_REWRITE_HOST_OSDEP_H
#define PRIM_REWRITE_HOST_OSDEP_H
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define g_free free
static inline void *g_realloc_n(void *old, size_t count, size_t size)
{
    assert(size && count <= SIZE_MAX / size);
    void *result = realloc(old, count * size);
    assert(result);
    return result;
}
#endif
