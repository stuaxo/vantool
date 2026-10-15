/* ring.c - vanplay's read-ahead frame ring.  See ring.h.
 *
 * Slots hold frames in SCREEN order (top row first), whatever the file's row order.  Rows are put in
 * place while loading, which happens in idle time; drawing a cached frame is then one contiguous block,
 * which the DOS backend can move from XMS straight into video memory (plat_draw_store).
 */
#include <stdio.h>
#include <stdlib.h>
#include "ring.h"
#include "store.h"

typedef struct {
    long frame;       /* -1 = empty */
    u32 rows;         /* file rows loaded so far (in file order) */
} Slot;

static VanFile *van;
static Slot *slots;
static u32 nslots, slot_size, next, window;
static unsigned chunk_rows;
static const char *why = "";
static unsigned char *buf;        /* one chunk of whole rows: borrows the VanFile's block cache */

int ring_init(VanFile *v, u32 budget, char *err)
{
    u32 got = 0, i;

    van = v;
    nslots = 0;
    slot_size = v->width * v->height;                 /* the image only (frame_size may add padding) */
    chunk_rows = (unsigned)(v->csize / v->width);
    if (budget < slot_size) {
        why = "budget below one frame";
        return 0;                                     /* stream from disk */
    }
    if (budget > slot_size * v->frames)
        budget = slot_size * v->frames;               /* no point reserving more than the file */
    /* Share the VanFile's I/O block instead of allocating another (up to 60 KB) of conventional memory;
     * the row cache it backs is invalidated whenever the ring uses it. */
    buf = v->cache;
    if (store_init(budget, &got)) {
        sprintf(err, "out of memory for frame buffer");
        return 1;
    }
    nslots = got / slot_size;
    if (!nslots) {
        why = got ? "free memory below one frame" : "no extended memory available";
        store_done();
        return 0;
    }
    slots = (Slot *)malloc((size_t)nslots * sizeof(Slot));
    if (!slots) {
        why = "no conventional memory for the slot table";
        nslots = 0;
        store_done();
        return 0;
    }
    why = nslots < v->frames ? "budget smaller than the file" : "";
    for (i = 0; i < nslots; i++) {
        slots[i].frame = -1;
        slots[i].rows = 0;
    }
    window = nslots < v->frames ? nslots : v->frames;
    next = 0;
    return 0;
}

u32 ring_slots(void) { return nslots; }
const char *ring_why(void) { return why; }
int ring_all_fit(void) { return nslots && nslots >= van->frames; }

void ring_want_from(u32 frame)
{
    next = frame % van->frames;
}

static int in_window(long f)
{
    return f >= 0 && ((u32)f + van->frames - next) % van->frames < window;
}

static long slot_of(u32 frame)
{
    u32 i;
    for (i = 0; i < nslots; i++)
        if (slots[i].frame == (long)frame)
            return (long)i;
    return -1;
}

int ring_has(u32 frame)
{
    long s = nslots ? slot_of(frame) : -1;
    return s >= 0 && slots[s].rows >= van->height;
}

long ring_offset(u32 frame)
{
    long s = slot_of(frame);
    return s >= 0 && slots[s].rows >= van->height ? (long)((u32)s * slot_size) : -1;
}

/* Slot for frame f: its own, else an empty one or one holding a frame no longer wanted. */
static long claim(u32 f)
{
    long s = slot_of(f);
    u32 j;
    if (s >= 0)
        return s;
    for (j = 0; j < nslots && in_window(slots[j].frame); j++)
        ;
    if (j == nslots)
        return -1;
    slots[j].frame = (long)f;
    slots[j].rows = 0;
    return (long)j;
}

static u32 screen_y(u32 k)                         /* file row k -> screen row */
{
    return van->pc ? k : van->height - 1 - k;
}

/* buf holds file rows k..k+n-1: store them at their screen positions in slot s.  Top-down files are
 * one contiguous block; bottom-up files are reversed, one move per row. */
static int put_rows(long s, u32 k, unsigned n)
{
    u32 base = (u32)s * slot_size;
    unsigned i;
    if (van->pc)
        return store_put(base + k * van->width, buf, (unsigned)(n * van->width));
    for (i = 0; i < n; i++)
        if (store_put(base + screen_y(k + i) * van->width, buf + (size_t)i * (size_t)van->width,
                      (unsigned)van->width))
            return 1;
    return 0;
}

/* Read file rows k..k+n-1 of frame f into buf. */
static int read_rows(u32 f, u32 k, unsigned n, int seek)
{
    if (seek && fseek(van->f, (long)(van->data_offset + f * van->frame_size + k * van->width), SEEK_SET) != 0)
        return 1;
    return fread(buf, 1, (size_t)n * (size_t)van->width, van->f) != (size_t)n * (size_t)van->width;
}

int ring_step(char *err)
{
    u32 i, f;
    unsigned n;
    long s;
    Slot *sl;

    for (i = 0; i < window; i++) {
        f = (next + i) % van->frames;
        s = slot_of(f);
        if (s >= 0 && slots[s].rows >= van->height)
            continue;                              /* already loaded */
        if (s < 0 && (s = claim(f)) < 0)
            return 0;
        sl = &slots[s];
        van->clen = 0;
        n = (unsigned)(van->height - sl->rows < chunk_rows ? van->height - sl->rows : chunk_rows);
        if (read_rows(f, sl->rows, n, 1) || put_rows(s, sl->rows, n)) {
            sprintf(err, "read error loading frame %lu", f);
            sl->frame = -1;
            return -1;
        }
        sl->rows += n;
        return 1;
    }
    return 0;
}

/* Screen rows y0..y0+count-1 of slot s to cb, a chunk at a time. */
static int draw_from_store(long s, u32 y0, u32 count, van_row_cb cb, void *ctx)
{
    u32 base = (u32)s * slot_size, y = y0;
    unsigned n, i;
    van->clen = 0;
    while (y < y0 + count) {
        n = (unsigned)(y0 + count - y < chunk_rows ? y0 + count - y : chunk_rows);
        if (store_get(base + y * van->width, buf, (unsigned)(n * van->width)))
            return 1;
        for (i = 0; i < n; i++)
            cb(ctx, y + i, buf + (size_t)i * (size_t)van->width);
        y += n;
    }
    return 0;
}

int ring_draw(u32 frame, van_row_cb cb, void *ctx, char *err)
{
    long s = slot_of(frame);
    if (s < 0 || draw_from_store(s, 0, van->height, cb, ctx)) {
        sprintf(err, "memory read error at frame %lu", frame);
        return 1;
    }
    return 0;
}

int ring_stream(u32 frame, van_row_cb cb, void *ctx, char *err)
{
    long s = claim(frame);                         /* -1: no slot free, just stream */
    u32 have = s >= 0 ? slots[s].rows : 0, k;
    unsigned n, i;

    /* the part already read ahead (file rows 0..have-1): from memory */
    if (have && draw_from_store(s, van->pc ? 0 : van->height - have, have, cb, ctx)) {
        sprintf(err, "memory read error at frame %lu", frame);
        return 1;
    }
    /* the rest: from disk, keeping a copy in the ring (write-through) */
    van->clen = 0;
    for (k = have; k < van->height; k += n) {
        n = (unsigned)(van->height - k < chunk_rows ? van->height - k : chunk_rows);
        if (read_rows(frame, k, n, k == have) || (s >= 0 && put_rows(s, k, n))) {
            sprintf(err, "read error at frame %lu", frame);
            if (s >= 0)
                slots[s].frame = -1;
            return 1;
        }
        for (i = 0; i < n; i++)
            cb(ctx, screen_y(k + i), buf + (size_t)i * (size_t)van->width);
        if (s >= 0)
            slots[s].rows = k + n;
    }
    return 0;
}

void ring_done(void)
{
    store_done();
    free(slots);
    slots = NULL;
    buf = NULL;
    nslots = 0;
}
