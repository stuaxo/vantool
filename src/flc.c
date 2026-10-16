/* flc.c - write FLI and FLC animations.  See flc.h.
 *
 * Chunks: COLOR_256 (FLC) or COLOR_64 (FLI) palette, BRUN for the first frame, then SS2 (word
 * packets) or LC (byte packets) for the rest.
 * Header times are left zero so the same input always gives the same file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flc.h"

#define FRAME_TYPE    0xF1FAu
#define CK_COLOR_256  4
#define CK_SS2        7
#define CK_COLOR_64   11
#define CK_LC         12
#define CK_BRUN       15
#define HEADER_SIZE   128

typedef struct {
    FILE *f;
    int fli, bad;
    u32 width, height;
    flc_row_fn get;
    void *ctx;
    char *err;
    unsigned char *prev, *cur, *line;
    long frame_pos, chunk_pos;
    unsigned chunks;
} Flc;

static void put8(Flc *w, unsigned v)
{
    if (putc((int)(v & 0xFF), w->f) == EOF)
        w->bad = 1;
}

static void put16(Flc *w, unsigned v)
{
    put8(w, v);
    put8(w, v >> 8);
}

static void put32(Flc *w, u32 v)
{
    put16(w, (unsigned)(v & 0xFFFF));
    put16(w, (unsigned)(v >> 16));
}

static void put_bytes(Flc *w, const unsigned char *p, unsigned n)
{
    if (n && fwrite(p, 1, n, w->f) != n)
        w->bad = 1;
}

/* Write a 16- or 32-bit value at pos, then carry on at the end of the file. */
static void patch(Flc *w, long pos, u32 v, int bytes)
{
    long end = ftell(w->f);
    if (end < 0 || fseek(w->f, pos, SEEK_SET) != 0) {
        w->bad = 1;
        return;
    }
    if (bytes == 2)
        put16(w, (unsigned)v);
    else
        put32(w, v);
    if (fseek(w->f, end, SEEK_SET) != 0)
        w->bad = 1;
}

static void frame_begin(Flc *w)
{
    w->frame_pos = ftell(w->f);
    w->chunks = 0;
    put32(w, 0);
    put16(w, FRAME_TYPE);
    put16(w, 0);
    put32(w, 0);
    put32(w, 0);
}

static void frame_end(Flc *w)
{
    patch(w, w->frame_pos, (u32)(ftell(w->f) - w->frame_pos), 4);
    patch(w, w->frame_pos + 6, w->chunks, 2);
}

static void chunk_begin(Flc *w, unsigned type)
{
    w->chunk_pos = ftell(w->f);
    w->chunks++;
    put32(w, 0);
    put16(w, type);
}

static void chunk_end(Flc *w)
{
    if ((ftell(w->f) - w->chunk_pos) & 1)
        put8(w, 0);                       /* chunks are kept to an even size */
    patch(w, w->chunk_pos, (u32)(ftell(w->f) - w->chunk_pos), 4);
}

static int same(const unsigned char *a, const unsigned char *b, unsigned unit)
{
    return a[0] == b[0] && (unit == 1 || a[1] == b[1]);
}

/* Encode n units (1 or 2 bytes each) of src as run packets: a signed count byte, then one unit to
 * repeat or the units to copy.  lit_pos: a positive count means copy (LC, SS2), else repeat (BRUN).
 * skip >= 0 puts a skip byte before each count, the first one holding skip.  Returns bytes written
 * and adds the packets to *packets. */
static unsigned runs(const unsigned char *src, unsigned n, unsigned unit, int lit_pos, int skip,
                     unsigned char *out, unsigned *packets)
{
    unsigned i = 0, o = 0, r, l;
    while (i < n) {
        for (r = 1; i + r < n && r < 127 && same(src + (i + r) * unit, src + i * unit, unit); r++)
            ;
        if (skip >= 0) {
            out[o++] = (unsigned char)skip;
            skip = 0;
        }
        if (r >= 3) {
            out[o++] = (unsigned char)(lit_pos ? 256 - r : r);
            memcpy(out + o, src + i * unit, unit);
            o += unit;
            i += r;
        } else {
            /* copy until a run of 3 or more starts */
            for (l = 1; i + l < n && l < 127; l++)
                if (i + l + 2 < n && same(src + (i + l) * unit, src + (i + l + 1) * unit, unit) &&
                    same(src + (i + l) * unit, src + (i + l + 2) * unit, unit))
                    break;
            out[o++] = (unsigned char)(lit_pos ? l : 256 - l);
            memcpy(out + o, src + i * unit, l * unit);
            o += l * unit;
            i += l;
        }
        (*packets)++;
    }
    return o;
}

/* Differences between two rows of n units as LC (unit 1) or SS2 (unit 2) packets.  Short unchanged
 * gaps are copied rather than skipped, as a new packet costs more.  LC counts packets in one byte, so
 * a line that needs more than 255 is sent as plain copies from its first change to its last.
 * Returns bytes, sets *packets. */
static unsigned delta_row(const unsigned char *prev, const unsigned char *cur, unsigned n,
                          unsigned unit, unsigned char *out, unsigned *packets)
{
    unsigned merge = unit == 1 ? 2 : 1, maxskip = 255 / unit, x = 0, o = 0, a, b, c;
    *packets = 0;
    for (;;) {
        for (a = x; a < n && same(prev + a * unit, cur + a * unit, unit); a++)
            ;
        if (a == n)
            break;
        for (b = c = a + 1; c < n && c - b < merge; c++)
            if (!same(prev + c * unit, cur + c * unit, unit))
                b = c + 1;
        /* a skip byte holds at most 255 bytes: step over longer gaps with a 1-unit copy */
        while (a - x > maxskip) {
            out[o++] = (unsigned char)(maxskip * unit);
            out[o++] = 1;
            memcpy(out + o, cur + (x + maxskip) * unit, unit);
            o += unit;
            x += maxskip + 1;
            (*packets)++;
        }
        o += runs(cur + a * unit, b - a, unit, 1, (int)((a - x) * unit), out + o, packets);
        x = b;
    }
    if (unit == 1 && *packets > 255) {
        for (a = 0; same(prev + a, cur + a, 1); a++)
            ;
        for (b = n; same(prev + b - 1, cur + b - 1, 1); b--)
            ;
        o = 0;
        *packets = 0;
        for (x = 0; a - x > 255; x += 256, (*packets)++) {
            out[o++] = 255;
            out[o++] = 1;
            out[o++] = cur[x + 255];
        }
        for (c = a; c < b; c += 127, (*packets)++) {
            unsigned l = b - c < 127 ? b - c : 127;
            out[o++] = (unsigned char)(c == a ? a - x : 0);
            out[o++] = (unsigned char)l;
            memcpy(out + o, cur + c, l);
            o += l;
        }
    }
    return o;
}

static int rows(Flc *w, u32 pf, u32 cf, u32 y)
{
    if ((pf != cf && w->get(w->ctx, pf, y, w->prev, w->err)) || w->get(w->ctx, cf, y, w->cur, w->err))
        return 1;
    return 0;
}

static int first_frame(Flc *w, const unsigned char *pal6)
{
    unsigned i, n, packets;
    u32 y;
    frame_begin(w);
    chunk_begin(w, w->fli ? CK_COLOR_64 : CK_COLOR_256);
    put16(w, 1);                          /* one packet: skip 0, count 0 = all 256 colours */
    put16(w, 0);
    for (i = 0; i < 768; i++) {
        unsigned c = pal6[i] & 63;
        put8(w, w->fli ? c : (c << 2) | (c >> 4));
    }
    chunk_end(w);
    chunk_begin(w, CK_BRUN);
    for (y = 0; y < w->height; y++) {
        if (rows(w, 0, 0, y))
            return 1;
        packets = 0;
        n = runs(w->cur, (unsigned)w->width, 1, 0, -1, w->line, &packets);
        put8(w, packets > 255 ? 0 : packets);     /* obsolete count; readers of wide images ignore it */
        put_bytes(w, w->line, n);
    }
    chunk_end(w);
    frame_end(w);
    return 0;
}

/* Frame cf stored as its differences from frame pf.  FLI uses LC.  FLC normally uses SS2, but SS2
 * keeps the last pixel of an odd-width line in a special word that common players (VistaPro's
 * PLAYFLC, ffmpeg) do not show correctly, so odd-width FLCs use LC as well. */
static int delta_frame(Flc *w, u32 pf, u32 cf)
{
    unsigned n, packets, odd = (unsigned)(w->width & 1);
    u32 y, lines = 0, skipped = 0;
    long lines_pos = 0;
    int started = 0, last_changed;
    frame_begin(w);
    for (y = 0; y < w->height; y++) {
        if (rows(w, pf, cf, y))
            return 1;
        if (w->fli || odd) {                                /* LC */
            n = delta_row(w->prev, w->cur, (unsigned)w->width, 1, w->line, &packets);
            if (!packets) {
                skipped++;
                continue;
            }
            if (!started) {
                chunk_begin(w, CK_LC);
                put16(w, (unsigned)y);                      /* first line changed */
                lines_pos = ftell(w->f);
                put16(w, 0);
                started = 1;
                skipped = 0;
            }
            for (; skipped; skipped--, lines++)
                put8(w, 0);                                 /* unchanged line inside the range */
            put8(w, packets);
        } else {                                            /* SS2 */
            n = delta_row(w->prev, w->cur, (unsigned)(w->width / 2), 2, w->line, &packets);
            last_changed = odd && w->prev[w->width - 1] != w->cur[w->width - 1];
            if (!packets && !last_changed) {
                skipped++;
                continue;
            }
            if (!started) {
                chunk_begin(w, CK_SS2);
                lines_pos = ftell(w->f);
                put16(w, 0);
                started = 1;
            }
            for (; skipped; skipped -= skipped > 16384 ? 16384 : skipped)
                put16(w, (unsigned)(0x10000L - (long)(skipped > 16384 ? 16384 : skipped)));
            if (last_changed)
                put16(w, 0x8000u | w->cur[w->width - 1]);
            put16(w, packets);
        }
        put_bytes(w, w->line, n);
        lines++;
    }
    if (started) {
        patch(w, lines_pos, lines, 2);
        chunk_end(w);
    }
    frame_end(w);
    return 0;
}

int flc_write(const char *path, int fli, u32 width, u32 height, u32 frames, u32 fps,
              const unsigned char *pal6, flc_row_fn get, void *ctx, char *err)
{
    static Flc w;
    u32 f, oframe2 = 0;
    int i, fail = 0;

    if (!frames || frames > 65535L || !width || !height || width > VAN_MAX_WIDTH || height > 65535L) {
        sprintf(err, "%.150s: cannot store %lu frames of %lux%lu", path, frames, width, height);
        return 1;
    }
    if (fps < 1)
        fps = 1;
    memset(&w, 0, sizeof(w));
    w.fli = fli;
    w.width = width;
    w.height = height;
    w.get = get;
    w.ctx = ctx;
    w.err = err;
    w.prev = (unsigned char *)malloc((size_t)width);
    w.cur = (unsigned char *)malloc((size_t)width);
    w.line = (unsigned char *)malloc((size_t)width * 2 + 64);
    if (!w.prev || !w.cur || !w.line) {
        sprintf(err, "out of memory");
        fail = 1;
    } else if (!(w.f = fopen(path, "wb"))) {
        sprintf(err, "cannot create %.150s", path);
        fail = 1;
    }
    if (!fail) {
        setvbuf(w.f, NULL, _IOFBF, VAN_IOBUF);
        for (i = 0; i < HEADER_SIZE; i++)
            put8(&w, 0);
        fail = first_frame(&w, pal6);
        for (f = 1; !fail && f <= frames; f++) {           /* f == frames: the ring frame */
            if (f == 1)
                oframe2 = (u32)ftell(w.f);
            fail = delta_frame(&w, f - 1, f == frames ? 0 : f);
        }
    }
    if (!fail) {
        patch(&w, 0, (u32)ftell(w.f), 4);
        patch(&w, 4, fli ? 0xAF11u : 0xAF12u, 2);
        patch(&w, 6, frames, 2);                           /* the ring frame is not counted */
        patch(&w, 8, width, 2);
        patch(&w, 10, height, 2);
        patch(&w, 12, 8, 2);                               /* bits per pixel */
        if (fli) {
            patch(&w, 16, (70 + fps / 2) / fps ? (70 + fps / 2) / fps : 1, 2);   /* 1/70 s */
        } else {
            patch(&w, 14, 3, 2);                           /* flags: file complete */
            patch(&w, 16, (1000 + fps / 2) / fps, 4);      /* milliseconds */
            patch(&w, 38, 1, 2);                           /* aspect 1:1, as VAN2FLC writes */
            patch(&w, 40, 1, 2);
            patch(&w, 80, HEADER_SIZE, 4);
            patch(&w, 84, oframe2, 4);
        }
        if (w.bad)
            sprintf(err, "write error on %.150s", path);
        fail = w.bad;
    }
    if (w.f && fclose(w.f) != 0 && !fail) {
        sprintf(err, "write error on %.150s", path);
        fail = 1;
    }
    if (fail && w.f)
        remove(path);
    free(w.prev);
    free(w.cur);
    free(w.line);
    return fail;
}
