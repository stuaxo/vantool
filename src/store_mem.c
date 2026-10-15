/* store_mem.c - vanplay frame store in ordinary heap memory (Linux). */
#include <stdlib.h>
#include <string.h>
#include "store.h"

static unsigned char *mem;

int store_init(u32 want, u32 *got)
{
    mem = want ? (unsigned char *)malloc((size_t)want + 2) : NULL;
    *got = mem ? want : 0;
    return 0;
}

int store_put(u32 off, const unsigned char *src, unsigned n)
{
    memcpy(mem + off, src, n);
    return 0;
}

int store_get(u32 off, unsigned char *dst, unsigned n)
{
    memcpy(dst, mem + off, n);
    return 0;
}

void store_done(void)
{
    free(mem);
    mem = NULL;
}

const char *store_kind(void)
{
    return "heap";
}
