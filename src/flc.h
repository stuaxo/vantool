/* flc.h - write FLI and FLC animations, one row at a time.
 *
 * Frames come from a callback, so no buffer exceeds a few image rows (fits 16-bit DOS).  The first
 * frame is stored whole (BRUN), later frames as differences from the one before (SS2, or LC for FLI
 * and odd widths), and a final "ring" frame takes the last frame back to the first so players can
 * loop smoothly, as VistaPro's VAN2FLC does.
 */
#ifndef FLC_H
#define FLC_H

#include "van.h"

/* Fill row (width bytes) with row y, counted from the top, of frame (0 .. frames-1). */
typedef int (*flc_row_fn)(void *ctx, u32 frame, u32 y, unsigned char *row, char *err);

/* fli: 1 writes FLI (320x200 only, speed in 1/70 s), 0 writes FLC.  pal6: 256 RGB triples, 6-bit. */
int flc_write(const char *path, int fli, u32 width, u32 height, u32 frames, u32 fps,
              const unsigned char *pal6, flc_row_fn get, void *ctx, char *err);

#endif
