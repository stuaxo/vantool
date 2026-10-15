/* store.h - bulk memory for vanplay's frame ring.
 * store_xms.c (DOS: XMS extended memory via HIMEM), store_mem.c (Linux: heap).
 * Offsets are bytes from the start of the store; lengths are under 64 KB, any parity.  On DOS the
 * memory pointer may be any real-mode address, including the VGA/VESA window at A000.
 */
#ifndef STORE_H
#define STORE_H

#include "van.h"


/* Reserve up to want bytes.  *got may be smaller, or 0 if none is available (not an error). */
int  store_init(u32 want, u32 *got);
int  store_put(u32 off, const unsigned char *src, unsigned n);   /* 0 on success */
int  store_get(u32 off, unsigned char *dst, unsigned n);
void store_done(void);                                           /* safe to call more than once */
const char *store_kind(void);                                    /* "XMS", "heap", ... */

#endif
