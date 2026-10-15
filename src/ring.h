/* ring.h - vanplay's read-ahead frame ring.
 *
 * A fixed memory budget (store.h) is split into frame slots.  The ring keeps the frames that will be
 * shown next, in playback order, and fills them a chunk at a time from ring_step(), which the player
 * calls whenever it would otherwise wait.  If every frame fits, nothing is ever evicted, and after the
 * first pass playback runs from memory.  DOS has no background disk I/O, so this cooperative filling is
 * how reading overlaps with display.
 */
#ifndef RING_H
#define RING_H

#include "van.h"

/* Size the ring from a byte budget.  Fewer than 1 slot is allowed: the player then streams from disk. */
int  ring_init(VanFile *v, u32 budget, char *err);
u32  ring_slots(void);
const char *ring_why(void);              /* why read-ahead is off or limited, for -v */
int  ring_all_fit(void);                 /* every frame has its own slot */
void ring_want_from(u32 frame);          /* frames from here on, in order (wrapping), are wanted next */
int  ring_step(char *err);               /* fill one chunk: 1 = did work, 0 = nothing to do, -1 = error */
int  ring_has(u32 frame);                /* frame fully loaded */
long ring_offset(u32 frame);             /* store offset of a loaded frame (screen order), -1 if not */
int  ring_draw(u32 frame, van_row_cb cb, void *ctx, char *err);   /* rows from memory, storage order */
/* Frame not loaded yet: read it from disk straight to cb, keeping a copy in the ring on the way
 * (write-through), so a late frame costs no extra copy out of memory. */
int  ring_stream(u32 frame, van_row_cb cb, void *ctx, char *err);
void ring_done(void);

#endif
