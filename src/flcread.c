/* flcread.c - read FLI and FLC animations.  See flcread.h.
 *
 * Chunks understood: COLOR_256 (4), SS2 (7), COLOR_64 (11), LC (12), BLACK (13), BRUN (15), COPY (16).
 * Others, such as the postage stamp, are skipped.  Every write is checked against the image size, so
 * a damaged file cannot write outside the rows.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flcread.h"

#define FRAME_TYPE    0xF1FAu
#define PREFIX_TYPE   0xF100u
#define CK_COLOR_256  4
#define CK_SS2        7
#define CK_COLOR_64   11
#define CK_LC         12
#define CK_BLACK      13
#define CK_BRUN       15
#define CK_COPY       16
#define HEADER_SIZE   128
#define MAX_HEIGHT    4096u

static unsigned rd8(FlcFile *c)
{
    int ch = getc(c->f);
    if (ch == EOF) {
        c->bad = 1;
        return 0;
    }
    return (unsigned)ch;
}

static unsigned rd16(FlcFile *c)
{
    unsigned lo = rd8(c);
    return lo | (rd8(c) << 8);
}

static u32 rd32(FlcFile *c)
{
    u32 lo = rd16(c);
    return lo | ((u32)rd16(c) << 16);
}

static int sbyte(unsigned v)
{
    return v > 127 ? (int)v - 256 : (int)v;
}

static void clear_picture(FlcFile *c)
{
    u32 y;
    for (y = 0; y < c->height; y++)
        memset(c->rows[y], 0, (size_t)c->width);
    memset(c->pal6, 0, sizeof(c->pal6));
    c->pal_gen++;
}

/* Palette chunk: packets of (colours to skip, count with 0 = 256, RGB triples).  COLOR_256 holds 8-bit
 * levels and COLOR_64 holds 6-bit ones.  Some files put 8-bit levels in COLOR_64 chunks, so once one has
 * a level above 63 the file's COLOR_64 chunks are all read as 8-bit. */
static void chunk_color(FlcFile *c, int is64)
{
    static unsigned char raw[256 * 3], has[256];
    unsigned packets = rd16(c), idx = 0, n, i, k, v, max = 0, shift;
    memset(has, 0, sizeof(has));
    while (packets-- && !c->bad) {
        idx += rd8(c);
        n = rd8(c);
        if (!n)
            n = 256;
        for (i = 0; i < n; i++, idx++) {
            for (k = 0; k < 3; k++) {
                v = rd8(c);
                if (v > max)
                    max = v;
                if (idx < 256) {
                    raw[idx * 3 + k] = (unsigned char)v;
                    has[idx] = 1;
                }
            }
        }
    }
    if (is64 && max > 63)
        c->pal8 = 1;
    shift = !is64 || c->pal8 ? 2 : 0;
    for (i = 0; i < 256; i++)
        if (has[i])
            for (k = 0; k < 3; k++)
                c->pal6[i * 3 + k] = (unsigned char)((raw[i * 3 + k] >> shift) & 63);
    c->pal_gen++;
}

/* BRUN: the first frame.  Each row: an obsolete packet count, then packets of a signed count: positive
 * repeats the next byte, negative copies that many bytes. */
static void chunk_brun(FlcFile *c)
{
    u32 y, x;
    for (y = 0; y < c->height && !c->bad; y++) {
        unsigned char *row = c->rows[y];
        (void)rd8(c);
        x = 0;
        while (x < c->width && !c->bad) {
            int n = sbyte(rd8(c)), i;
            if (n > 0) {
                unsigned v = rd8(c);
                for (i = 0; i < n; i++, x++)
                    if (x < c->width)
                        row[x] = (unsigned char)v;
            } else if (n < 0) {
                for (i = 0; i < -n; i++, x++) {
                    unsigned v = rd8(c);
                    if (x < c->width)
                        row[x] = (unsigned char)v;
                }
            } else {
                c->bad = 1;                       /* a zero count never advances */
            }
        }
    }
}

/* LC (FLI deltas): first line, number of lines; each line has a packet count, then packets of a skip
 * byte and a signed count: positive copies that many bytes, negative repeats the next byte. */
static void chunk_lc(FlcFile *c)
{
    u32 y = rd16(c), lines = rd16(c), x;
    unsigned packets;
    while (lines-- && !c->bad) {
        unsigned char *row = y < c->height ? c->rows[y] : NULL;
        packets = rd8(c);
        x = 0;
        while (packets-- && !c->bad) {
            int n;
            x += rd8(c);
            n = sbyte(rd8(c));
            if (n > 0) {
                int i;
                for (i = 0; i < n; i++, x++) {
                    unsigned v = rd8(c);
                    if (row && x < c->width)
                        row[x] = (unsigned char)v;
                }
            } else if (n < 0) {
                int i;
                unsigned v = rd8(c);
                for (i = 0; i < -n; i++, x++)
                    if (row && x < c->width)
                        row[x] = (unsigned char)v;
            }
        }
        y++;
    }
}

/* SS2 (FLC deltas): number of lines, then per line some words: 11xxxxxx skips lines, 10xxxxxx gives the
 * last pixel of an odd-width line, 00xxxxxx is the packet count.  Packets: a skip byte (pixels) and a
 * signed word count: positive copies that many words, negative repeats the next word. */
static void chunk_ss2(FlcFile *c)
{
    u32 lines = rd16(c), y = 0, x;
    unsigned w, packets;
    while (lines && !c->bad) {
        unsigned char *row;
        w = rd16(c);
        if ((w & 0xC000u) == 0xC000u) {
            y += 0x10000L - w;
            continue;
        }
        if ((w & 0xC000u) == 0x8000u) {
            if (y < c->height)
                c->rows[y][c->width - 1] = (unsigned char)(w & 0xFF);
            continue;
        }
        if (w & 0xC000u) {
            c->bad = 1;                           /* 01xxxxxx is undefined */
            break;
        }
        row = y < c->height ? c->rows[y] : NULL;
        packets = w;
        x = 0;
        while (packets-- && !c->bad) {
            int n, i;
            x += rd8(c);
            n = sbyte(rd8(c));
            if (n > 0) {
                for (i = 0; i < n; i++, x += 2) {
                    unsigned a = rd8(c), b = rd8(c);
                    if (row && x < c->width)
                        row[x] = (unsigned char)a;
                    if (row && x + 1 < c->width)
                        row[x + 1] = (unsigned char)b;
                }
            } else if (n < 0) {
                unsigned a = rd8(c), b = rd8(c);
                for (i = 0; i < -n; i++, x += 2) {
                    if (row && x < c->width)
                        row[x] = (unsigned char)a;
                    if (row && x + 1 < c->width)
                        row[x + 1] = (unsigned char)b;
                }
            }
        }
        y++;
        lines--;
    }
}

static void chunk_copy(FlcFile *c)
{
    u32 y, x;
    for (y = 0; y < c->height && !c->bad; y++)
        for (x = 0; x < c->width; x++)
            c->rows[y][x] = (unsigned char)rd8(c);
}

/* Decode the frame at c->pos and move on to the one after.  Frames that are not pictures (the
 * 0xF100 prefix) are passed over.  Returns 0 on success. */
static int decode_next(FlcFile *c, char *err)
{
    long start, cstart;
    u32 fsize, csize;
    unsigned type, chunks, ctype, i;

    for (;;) {
        start = c->pos;
        if (start + 16 > c->flen || fseek(c->f, start, SEEK_SET) != 0) {
            sprintf(err, "unexpected end of animation at offset %ld", start);
            return 1;
        }
        fsize = rd32(c);
        type = rd16(c);
        chunks = rd16(c);
        if (c->bad || fsize < 16 || start + (long)fsize > c->flen) {
            sprintf(err, "damaged frame at offset %ld", start);
            return 1;
        }
        c->pos = start + (long)fsize;
        if (type == FRAME_TYPE)
            break;
        if (type != PREFIX_TYPE) {
            sprintf(err, "bad frame header at offset %ld", start);
            return 1;
        }
    }
    cstart = start + 16;
    for (i = 0; i < chunks; i++) {
        if (fseek(c->f, cstart, SEEK_SET) != 0)
            break;
        csize = rd32(c);
        ctype = rd16(c);
        if (c->bad || csize < 6 || cstart + (long)csize > c->pos) {
            sprintf(err, "damaged chunk at offset %ld", cstart);
            return 1;
        }
        switch (ctype) {
        case CK_COLOR_256: chunk_color(c, 0); break;
        case CK_COLOR_64:  chunk_color(c, 1); break;
        case CK_SS2:       chunk_ss2(c); break;
        case CK_LC:        chunk_lc(c); break;
        case CK_BRUN:      chunk_brun(c); break;
        case CK_COPY:      chunk_copy(c); break;
        case CK_BLACK: {
            u32 y;
            for (y = 0; y < c->height; y++)
                memset(c->rows[y], 0, (size_t)c->width);
            break;
        }
        default: break;
        }
        if (c->bad) {
            sprintf(err, "damaged data in the chunk at offset %ld", cstart);
            return 1;
        }
        cstart += (long)csize;
    }
    return 0;
}

int flcr_probe(const char *path)
{
    unsigned char h[6];
    FILE *f = fopen(path, "rb");
    int ok;
    if (!f)
        return 0;
    ok = fread(h, 1, sizeof(h), f) == sizeof(h) && h[5] == 0xAF && (h[4] == 0x11 || h[4] == 0x12);
    fclose(f);
    return ok;
}

void flcr_close(FlcFile *c)
{
    u32 y;
    if (c->f)
        fclose(c->f);
    c->f = NULL;
    if (c->rows) {
        for (y = 0; y < c->height; y++)
            free(c->rows[y]);
        free(c->rows);
    }
    c->rows = NULL;
}

int flcr_open(FlcFile *c, const char *path, char *err)
{
    unsigned magic, depth;
    u32 y, speed;

    memset(c, 0, sizeof(*c));
    c->cur = -1;
    if (!(c->f = fopen(path, "rb"))) {
        sprintf(err, "cannot open %.150s", path);
        return 1;
    }
    setvbuf(c->f, NULL, _IOFBF, VAN_IOBUF);
    if (fseek(c->f, 0, SEEK_END) != 0 || (c->flen = ftell(c->f)) < HEADER_SIZE || fseek(c->f, 4, SEEK_SET) != 0) {
        sprintf(err, "%.150s is too short to be an animation", path);
        flcr_close(c);
        return 1;
    }
    magic = rd16(c);
    c->frames = rd16(c);
    c->width = rd16(c);
    c->height = rd16(c);
    depth = rd16(c);
    (void)rd16(c);                                 /* flags */
    c->fli = magic == 0xAF11u;
    if (magic != 0xAF11u && magic != 0xAF12u) {
        sprintf(err, "%.150s is not an FLI or FLC file", path);
        flcr_close(c);
        return 1;
    }
    if (c->fli) {
        speed = rd16(c);                           /* 1/70 s */
        c->delay_ms = speed * 1000UL / 70;
        c->first_pos = HEADER_SIZE;
        if (!c->width)
            c->width = 320;
        if (!c->height)
            c->height = 200;
    } else {
        c->delay_ms = rd32(c);                     /* ms */
        if (fseek(c->f, 80, SEEK_SET) != 0)
            c->bad = 1;
        c->first_pos = (long)rd32(c);
        if (c->first_pos < HEADER_SIZE || c->first_pos >= c->flen)
            c->first_pos = HEADER_SIZE;
    }
    if (c->bad || (depth && depth != 8)) {
        sprintf(err, "%.150s: only 8-bit animations are supported", path);
        flcr_close(c);
        return 1;
    }
    if (!c->frames || !c->width || c->width > VAN_MAX_WIDTH || !c->height || c->height > MAX_HEIGHT) {
        sprintf(err, "%.150s: cannot play %lu frames of %lux%lu", path, c->frames, c->width, c->height);
        flcr_close(c);
        return 1;
    }
    c->rows = (unsigned char **)calloc((size_t)c->height, sizeof(unsigned char *));
    for (y = 0; c->rows && y < c->height; y++)
        if (!(c->rows[y] = (unsigned char *)malloc((size_t)c->width)))
            break;
    if (!c->rows || y < c->height) {
        sprintf(err, "out of memory for a %lux%lu picture", c->width, c->height);
        flcr_close(c);
        return 1;
    }
    clear_picture(c);
    c->pos = c->first_pos;
    c->second_pos = c->first_pos;
    return 0;
}

int flcr_seek(FlcFile *c, u32 frame, char *err)
{
    if ((long)frame == c->cur)
        return 0;
    if (frame >= c->frames) {
        sprintf(err, "no frame %lu", frame);
        return 1;
    }
    if ((long)frame == c->cur + 1 || (frame == 0 && c->cur == (long)c->frames - 1 && c->pos + 16 <= c->flen)) {
        /* the next frame, or from the last back to the first by the ring frame */
        if (decode_next(c, err))
            return 1;
        if (frame == 0) {
            if (c->cur >= 0)
                c->pos = c->second_pos;            /* after the ring frame, carry on from frame 1 */
            else
                c->second_pos = c->pos;            /* the first frame, decoded for the first time */
        }
        c->cur = (long)frame;
        return 0;
    }
    clear_picture(c);                              /* start again from the first frame */
    c->pos = c->first_pos;
    c->cur = -1;
    while (c->cur < (long)frame) {
        if (decode_next(c, err))
            return 1;
        if (++c->cur == 0)
            c->second_pos = c->pos;
    }
    return 0;
}
