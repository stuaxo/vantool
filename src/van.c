/* van.c - VistaPro VANIM (.VAN) reader/writer.  See docs/VAN_SPEC.md. */
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include "van.h"

static const char MAGIC_PC[8] = { 'V','A','N','I','M','P','C',0 };
static const char MAGIC_WN[8] = { 'V','A','N','I','M','W','N',0 };

const VanMode van_modes[] = {
    { 0x13,  320,  200 }, { 0x100, 640, 400 }, { 0x101, 640, 480 },
    { 0x103, 800,  600 }, { 0x105, 1024, 768 }, { 0x107, 1280, 1024 }
};
const int van_mode_count = sizeof(van_modes) / sizeof(van_modes[0]);

unsigned char *van_alloc_block(unsigned min, unsigned *size)
{
    static const unsigned sizes[] = { VAN_CACHE, 32768u, 16384u, 8192u, 4096u };
    unsigned char *p;
    int i;
    for (i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++) {
        if (sizes[i] < min)
            break;
        if ((p = (unsigned char *)malloc(sizes[i])) != NULL) {
            /* leave some heap for the small allocations that follow (tables, stdio); take a
             * smaller block rather than the last few KB of a 640 KB machine */
            unsigned char *spare = (unsigned char *)malloc(4096u);
            if (!spare && i + 1 < (int)(sizeof(sizes) / sizeof(sizes[0])) && sizes[i + 1] >= min) {
                free(p);
                continue;
            }
            free(spare);
            *size = sizes[i];
            return p;
        }
    }
    *size = 0;
    return NULL;
}

u32 van_get32(const unsigned char *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

void van_put32(unsigned char *p, u32 v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

u32 van_pick_mode(u32 width, u32 height, int *exact)
{
    int i;
    u32 best = 0, best_area = 0;
    for (i = 0; i < van_mode_count; i++) {
        if (van_modes[i].width == width && van_modes[i].height == height) {
            *exact = 1;
            return van_modes[i].mode;
        }
    }
    *exact = 0;
    for (i = 0; i < van_mode_count; i++) {
        u32 area = van_modes[i].width * van_modes[i].height;
        if (van_modes[i].width >= width && van_modes[i].height >= height && (!best || area < best_area)) {
            best = van_modes[i].mode;
            best_area = area;
        }
    }
    return best;
}

int van_open(VanFile *v, const char *path, char *err)
{
    long size;
    u32 body;

    memset(v, 0, sizeof(*v));
    v->f = fopen(path, "rb");
    if (v->f)                       /* all frame I/O goes through our own block cache */
        setvbuf(v->f, NULL, _IONBF, 0);
    if (!v->f) {
        sprintf(err, "cannot open %.150s: %.60s", path, strerror(errno));
        return 1;
    }
    if (fread(v->header, 1, VAN_HEADER_SIZE, v->f) != VAN_HEADER_SIZE ||
        fread(v->palette, 1, VAN_PALETTE_SIZE, v->f) != VAN_PALETTE_SIZE) {
        sprintf(err, "%.150s: too short for a VANIM header", path);
        goto fail;
    }
    if (memcmp(v->header, MAGIC_PC, 8) == 0)
        v->pc = 1;
    else if (memcmp(v->header, MAGIC_WN, 8) != 0) {
        sprintf(err, "%.150s: not a VANIM file (bad magic)", path);
        goto fail;
    }
    v->image_bytes = van_get32(v->header + 0x08);
    v->data_offset = van_get32(v->header + 0x0C);
    v->frame_size  = van_get32(v->header + 0x10);
    v->colors      = van_get32(v->header + 0x14);
    v->width       = van_get32(v->header + 0x18);
    v->height      = van_get32(v->header + 0x1C);
    v->unk20       = van_get32(v->header + 0x20);
    v->video_mode  = van_get32(v->header + 0x24);
    if (v->width == 0 || v->height == 0 || v->width > VAN_MAX_WIDTH ||
        v->width * v->height > v->frame_size) {
        sprintf(err, "%.150s: bad geometry %lux%lu, frame_size %lu", path, v->width, v->height, v->frame_size);
        goto fail;
    }
    if (fseek(v->f, 0L, SEEK_END) != 0 || (size = ftell(v->f)) < 0) {
        sprintf(err, "%.150s: cannot get file size", path);
        goto fail;
    }
    body = (u32)size > v->data_offset ? (u32)size - v->data_offset : 0;
    v->frames = body / v->frame_size;
    v->trailing = body % v->frame_size;
    v->cache = van_alloc_block(v->width + 2 > 4096u ? (unsigned)v->width + 2 : 4096u, &v->csize);
    if (!v->cache) {
        sprintf(err, "out of memory");
        goto fail;
    }
    v->clen = 0;
    v->last = -1;
    return 0;
fail:
    fclose(v->f);
    v->f = NULL;
    return 1;
}

/* Load the cache block holding [pos, pos+need).  Walking backwards, the block ends at the
 * requested row so the following rows (earlier in the file) are already in it. */
static int cache_fill(VanFile *v, long pos, unsigned need)
{
    long start = (v->last >= 0 && pos < v->last) ? pos + (long)need - (long)v->csize : pos;
    if (start < (long)v->data_offset)
        start = (long)v->data_offset;
    if (start > pos)
        start = pos;
    v->clen = 0;
    if (fseek(v->f, start, SEEK_SET) != 0)
        return 1;
    v->clen = (unsigned)fread(v->cache, 1, v->csize, v->f);
    v->cstart = start;
    return pos + (long)need > start + (long)v->clen;
}

int van_read_row(VanFile *v, u32 frame, u32 y, unsigned char *row, char *err)
{
    u32 stored = v->pc ? y : v->height - 1 - y;
    long pos = (long)(v->data_offset + frame * v->frame_size + stored * v->width);
    unsigned w = (unsigned)v->width;
    if (frame >= v->frames || y >= v->height) {
        sprintf(err, "frame %lu row %lu out of range", frame, y);
        return 1;
    }
    if (!(v->clen && pos >= v->cstart && pos + (long)w <= v->cstart + (long)v->clen) &&
        cache_fill(v, pos, w)) {
        sprintf(err, "read error at frame %lu row %lu", frame, y);
        return 1;
    }
    memcpy(row, v->cache + (unsigned)(pos - v->cstart), w);
    v->last = pos;
    return 0;
}

int van_read_frame(VanFile *v, u32 frame, unsigned char *rowbuf, van_row_cb cb, void *ctx, char *err)
{
    /* Storage order, whole rows per read, rows handed to cb straight from the cache: one DOS read
     * per block and no per-row copy.  (rowbuf is unused; kept for API symmetry.) */
    unsigned per = (unsigned)(v->csize / v->width), n = 0, i;
    u32 k = 0, y;
    long pos;
    (void)rowbuf;
    if (frame >= v->frames) {
        sprintf(err, "frame %lu out of range", frame);
        return 1;
    }
    pos = (long)(v->data_offset + frame * v->frame_size);
    if (fseek(v->f, pos, SEEK_SET) != 0) {
        sprintf(err, "seek error at frame %lu", frame);
        return 1;
    }
    while (k < v->height) {
        n = (unsigned)(v->height - k < per ? v->height - k : per);
        if (fread(v->cache, 1, (size_t)n * (size_t)v->width, v->f) != (size_t)n * (size_t)v->width) {
            sprintf(err, "read error at frame %lu", frame);
            v->clen = 0;
            return 1;
        }
        for (i = 0; i < n; i++, k++) {
            y = v->pc ? k : v->height - 1 - k;
            cb(ctx, y, v->cache + (size_t)i * (size_t)v->width);
        }
    }
    v->cstart = pos + (long)(k - n) * (long)v->width;   /* cache now holds the frame's last block */
    v->clen = n * (unsigned)v->width;
    v->last = -1;
    return 0;
}

void van_close(VanFile *v)
{
    if (v->f)
        fclose(v->f);
    v->f = NULL;
    free(v->cache);
    v->cache = NULL;
}

int van_create(VanWriter *w, const char *path, u32 width, u32 height,
               const unsigned char *palette6, u32 video_mode, int pc, char *err)
{
    unsigned char hdr[VAN_HEADER_SIZE];
    int i;

    memset(w, 0, sizeof(*w));
    if (width == 0 || height == 0 || width > VAN_MAX_WIDTH) {
        sprintf(err, "bad image size %lux%lu", width, height);
        return 1;
    }
    for (i = 0; i < VAN_PALETTE_SIZE; i++) {
        if (palette6[i] > 63) {
            sprintf(err, "palette entry %d is not a 6-bit value", i / 3);
            return 1;
        }
    }
    w->f = fopen(path, "wb");
    if (w->f)
        setvbuf(w->f, NULL, _IONBF, 0);
    if (!w->f) {
        sprintf(err, "cannot create %.150s: %.60s", path, strerror(errno));
        return 1;
    }
    w->pc = pc;
    w->width = width;
    w->height = height;
    /* Field values exactly as VP3.EXE 3.20 writes them (spec section 8). */
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, pc ? MAGIC_PC : MAGIC_WN, 8);
    van_put32(hdr + 0x08, width * height);
    van_put32(hdr + 0x0C, VAN_DATA_OFFSET);
    van_put32(hdr + 0x10, width * height);
    van_put32(hdr + 0x14, 256);
    van_put32(hdr + 0x18, width);
    van_put32(hdr + 0x1C, height);
    van_put32(hdr + 0x20, 1);
    van_put32(hdr + 0x24, video_mode);
    if (fwrite(hdr, 1, sizeof(hdr), w->f) != sizeof(hdr) ||
        fwrite(palette6, 1, VAN_PALETTE_SIZE, w->f) != VAN_PALETTE_SIZE) {
        sprintf(err, "write error on %.150s", path);
        fclose(w->f);
        w->f = NULL;
        return 1;
    }
    w->cache = van_alloc_block(width > 4096u ? (unsigned)width : 4096u, &w->csize);
    if (!w->cache) {
        sprintf(err, "out of memory");
        fclose(w->f);
        w->f = NULL;
        return 1;
    }
    w->cvalid = 0;
    w->dlo = w->dhi = 0;
    w->last = -1;
    return 0;
}

/* Write out the dirty part of the block: one seek and one write. */
static int cache_flush(VanWriter *w)
{
    unsigned n = (unsigned)(w->dhi - w->dlo);
    int bad = 0;
    if (n)
        bad = fseek(w->f, w->dlo, SEEK_SET) != 0 ||
              fwrite(w->cache + (unsigned)(w->dlo - w->cstart), 1, n, w->f) != n;
    w->dlo = w->dhi = 0;
    return bad;
}

int van_write_row(VanWriter *w, u32 y, const unsigned char *row, char *err)
{
    u32 stored = w->pc ? y : w->height - 1 - y;
    long pos = (long)(VAN_DATA_OFFSET + w->frames * w->width * w->height + stored * w->width);
    long end = pos + (long)w->width;
    int clean = w->dlo == w->dhi;
    if (y >= w->height) {
        sprintf(err, "row %lu out of range", y);
        return 1;
    }
    /* Rows must land in the current block and extend the dirty range without leaving a gap. */
    if (!(w->cvalid && pos >= w->cstart && end <= w->cstart + (long)w->csize &&
          (clean || pos == w->dhi || end == w->dlo))) {
        if (cache_flush(w)) {
            sprintf(err, "write error at frame %lu (disk full?)", w->frames);
            return 1;
        }
        /* writing backwards: place the block so that it ends at this row */
        w->cstart = (w->last >= 0 && pos < w->last) ? end - (long)w->csize : pos;
        if (w->cstart < VAN_DATA_OFFSET)
            w->cstart = VAN_DATA_OFFSET;
        w->cvalid = 1;
        clean = 1;
    }
    memcpy(w->cache + (unsigned)(pos - w->cstart), row, (size_t)w->width);
    if (clean) {
        w->dlo = pos;
        w->dhi = end;
    } else if (pos == w->dhi) {
        w->dhi = end;
    } else {
        w->dlo = pos;
    }
    w->last = pos;
    return 0;
}

void van_end_frame(VanWriter *w)
{
    w->frames++;
}

int van_close_writer(VanWriter *w, char *err)
{
    int bad = 0;
    if (w->f) {
        bad = cache_flush(w);
        bad |= ferror(w->f) | fclose(w->f);
        w->f = NULL;
    }
    free(w->cache);
    w->cache = NULL;
    if (bad)
        sprintf(err, "write error while closing");
    return bad;
}
