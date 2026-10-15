/* img.c - 8-bit palettized BMP and PCX, read and write, one row at a time. */
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include "img.h"

static unsigned get16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

int img_type_from_name(const char *path)
{
    const char *dot = strrchr(path, '.');
    char ext[4];
    int i;
    if (!dot || strlen(dot) != 4)
        return 0;
    for (i = 0; i < 3; i++)
        ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    ext[3] = 0;
    if (strcmp(ext, "bmp") == 0) return IMG_BMP;
    if (strcmp(ext, "pcx") == 0) return IMG_PCX;
    return 0;
}

/* ---------- BMP ---------- */

static int bmp_open(ImgReader *r, const char *path, char *err)
{
    unsigned char h[54], q[4];
    u32 hdr_size, colors, i;
    long height;

    if (fread(h, 1, 54, r->f) != 54 || h[0] != 'B' || h[1] != 'M') {
        sprintf(err, "%.150s: not a BMP file", path);
        return 1;
    }
    hdr_size = van_get32(h + 14);
    r->bits_offset = (long)van_get32(h + 10);
    r->width = van_get32(h + 18);
    height = (long)van_get32(h + 22);       /* signed: negative = top-down */
    if (get16(h + 28) != 8 || van_get32(h + 30) != 0 || hdr_size < 40) {
        sprintf(err, "%.150s: only uncompressed 8-bit BMP is supported", path);
        return 1;
    }
    r->top_down = height < 0;
    r->height = (u32)(height < 0 ? -height : height);
    colors = van_get32(h + 46);
    if (colors == 0 || colors > 256)
        colors = 256;
    memset(r->palette, 0, sizeof(r->palette));
    fseek(r->f, 14L + (long)hdr_size, SEEK_SET);
    for (i = 0; i < colors; i++) {                     /* stored as B,G,R,0 */
        if (fread(q, 1, 4, r->f) != 4) {
            sprintf(err, "%.150s: truncated palette", path);
            return 1;
        }
        r->palette[i * 3] = q[2];
        r->palette[i * 3 + 1] = q[1];
        r->palette[i * 3 + 2] = q[0];
    }
    r->stride = (r->width + 3) & ~3UL;
    r->pos = -1;
    return 0;
}

static int bmp_read_row(ImgReader *r, u32 y, unsigned char *row, char *err)
{
    u32 stored = r->top_down ? y : r->height - 1 - y;
    long pos = r->bits_offset + (long)(stored * r->stride);
    if ((pos != r->pos && fseek(r->f, pos, SEEK_SET) != 0) ||
        fread(row, 1, (size_t)r->width, r->f) != (size_t)r->width) {
        sprintf(err, "BMP read error at row %lu", y);
        r->pos = -1;
        return 1;
    }
    r->pos = pos + (long)r->width;
    if (r->stride > r->width) {      /* read the 1-3 padding bytes too, to stay sequential */
        unsigned char pad[3];
        if (fread(pad, 1, (size_t)(r->stride - r->width), r->f) == (size_t)(r->stride - r->width))
            r->pos += (long)(r->stride - r->width);
    }
    return 0;
}

static int bmp_write(FILE *f, u32 w, u32 h, const unsigned char *pal8,
                     img_row_fn get_row, void *ctx, unsigned char *row, char *err)
{
    static unsigned char hd[54];
    unsigned char q[4];
    u32 stride = (w + 3) & ~3UL, y;
    static const unsigned char pad[3] = { 0, 0, 0 };
    int i;

    memset(hd, 0, sizeof(hd));
    hd[0] = 'B'; hd[1] = 'M';
    van_put32(hd + 2, 54 + 1024 + stride * h);
    van_put32(hd + 10, 54 + 1024);
    van_put32(hd + 14, 40);
    van_put32(hd + 18, w);
    van_put32(hd + 22, h);
    put16(hd + 26, 1);
    put16(hd + 28, 8);
    van_put32(hd + 34, stride * h);
    van_put32(hd + 38, 2835);                   /* 72 dpi */
    van_put32(hd + 42, 2835);
    van_put32(hd + 46, 256);
    fwrite(hd, 1, 54, f);
    for (i = 0; i < 256; i++) {
        q[0] = pal8[i * 3 + 2]; q[1] = pal8[i * 3 + 1]; q[2] = pal8[i * 3]; q[3] = 0;
        fwrite(q, 1, 4, f);
    }
    for (y = h; y-- > 0;) {                     /* bottom-up */
        if (get_row(ctx, y, row, err))
            return 1;
        fwrite(row, 1, (size_t)w, f);
        fwrite(pad, 1, (size_t)(stride - w), f);
    }
    return 0;
}

/* ---------- PCX (version 5, 8 bits, 1 plane, RLE, 256-colour palette at the end) ---------- */

static int pcx_open(ImgReader *r, const char *path, char *err)
{
    static unsigned char h[128];
    unsigned char tag;
    long size;

    if (fread(h, 1, 128, r->f) != 128 || h[0] != 0x0A || h[2] != 1) {
        sprintf(err, "%.150s: not an RLE PCX file", path);
        return 1;
    }
    if (h[3] != 8 || h[65] != 1) {
        sprintf(err, "%.150s: only 8-bit single-plane PCX is supported", path);
        return 1;
    }
    r->width = (u32)get16(h + 8) - get16(h + 4) + 1;
    r->height = (u32)get16(h + 10) - get16(h + 6) + 1;
    r->bytes_per_line = get16(h + 66);
    if (r->bytes_per_line < r->width) {
        sprintf(err, "%.150s: bad bytes-per-line", path);
        return 1;
    }
    fseek(r->f, 0L, SEEK_END);
    size = ftell(r->f);
    if (size < 128 + 769 || fseek(r->f, size - 769, SEEK_SET) != 0 ||
        fread(&tag, 1, 1, r->f) != 1 || tag != 0x0C || fread(r->palette, 1, 768, r->f) != 768) {
        sprintf(err, "%.150s: no 256-colour palette at end of PCX", path);
        return 1;
    }
    fseek(r->f, 128L, SEEK_SET);
    r->next_row = 0;
    r->run_count = 0;
    return 0;
}

static int pcx_read_row(ImgReader *r, u32 y, unsigned char *row, char *err)
{
    u32 x;
    int c;
    if (y != r->next_row) {
        sprintf(err, "PCX rows must be read in order");
        return 1;
    }
    for (x = 0; x < r->bytes_per_line; x++) {
        if (r->run_count == 0) {
            if ((c = getc(r->f)) == EOF)
                goto eof;
            if ((c & 0xC0) == 0xC0) {
                r->run_count = c & 0x3F;
                if ((c = getc(r->f)) == EOF)
                    goto eof;
            } else {
                r->run_count = 1;
            }
            r->run_value = (unsigned char)c;
        }
        if (x < r->width)
            row[x] = r->run_value;
        r->run_count--;
    }
    r->next_row++;
    return 0;
eof:
    sprintf(err, "PCX data ends early at row %lu", y);
    return 1;
}

static int pcx_write(FILE *f, u32 w, u32 h, const unsigned char *pal8,
                     img_row_fn get_row, void *ctx, unsigned char *row, char *err)
{
    static unsigned char hd[128];
    static unsigned char enc[2 * VAN_MAX_WIDTH + 4];   /* worst case: 2 bytes per pixel */
    u32 bpl = (w + 1) & ~1UL, x, y;
    unsigned o;

    memset(hd, 0, sizeof(hd));
    hd[0] = 0x0A; hd[1] = 5; hd[2] = 1; hd[3] = 8;
    put16(hd + 8, (unsigned)(w - 1));
    put16(hd + 10, (unsigned)(h - 1));
    put16(hd + 12, 72);
    put16(hd + 14, 72);
    hd[65] = 1;
    put16(hd + 66, (unsigned)bpl);
    put16(hd + 68, 1);
    fwrite(hd, 1, 128, f);
    for (y = 0; y < h; y++) {
        if (get_row(ctx, y, row, err))
            return 1;
        if (bpl > w)
            row[w] = 0;
        for (x = 0, o = 0; x < bpl;) {     /* encode the row into enc[], then one fwrite */
            unsigned char v = row[x];
            unsigned n = 1;
            while (x + n < bpl && n < 63 && row[x + n] == v)
                n++;
            if (n > 1 || (v & 0xC0) == 0xC0)
                enc[o++] = (unsigned char)(0xC0 | n);
            enc[o++] = v;
            x += n;
        }
        fwrite(enc, 1, o, f);
    }
    putc(0x0C, f);
    fwrite(pal8, 1, 768, f);
    return 0;
}

/* ---------- common ---------- */

int img_open(ImgReader *r, const char *path, char *err)
{
    int rc;
    memset(r, 0, sizeof(*r));
    r->type = img_type_from_name(path);
    if (!r->type) {
        sprintf(err, "%.150s: unknown image type (use .bmp or .pcx)", path);
        return 1;
    }
    r->f = fopen(path, "rb");
    if (r->f)
        setvbuf(r->f, NULL, _IOFBF, VAN_IOBUF);
    if (!r->f) {
        sprintf(err, "cannot open %.150s: %.60s", path, strerror(errno));
        return 1;
    }
    rc = r->type == IMG_BMP ? bmp_open(r, path, err) : pcx_open(r, path, err);
    if (!rc && (r->width == 0 || r->height == 0 || r->width > VAN_MAX_WIDTH)) {
        sprintf(err, "%.150s: unsupported size %lux%lu", path, r->width, r->height);
        rc = 1;
    }
    if (rc)
        img_close(r);
    return rc;
}

int img_bottom_up(const ImgReader *r)
{
    return r->type == IMG_BMP && !r->top_down;
}

int img_read_row(ImgReader *r, u32 y, unsigned char *row, char *err)
{
    return r->type == IMG_BMP ? bmp_read_row(r, y, row, err) : pcx_read_row(r, y, row, err);
}

void img_close(ImgReader *r)
{
    if (r->f)
        fclose(r->f);
    r->f = NULL;
}

int img_write(const char *path, int type, u32 width, u32 height, const unsigned char *palette8,
              img_row_fn get_row, void *ctx, unsigned char *rowbuf, char *err)
{
    FILE *f = fopen(path, "wb");
    int rc;
    if (f)
        setvbuf(f, NULL, _IOFBF, VAN_IOBUF);
    if (!f) {
        sprintf(err, "cannot create %.150s: %.60s", path, strerror(errno));
        return 1;
    }
    rc = type == IMG_BMP ? bmp_write(f, width, height, palette8, get_row, ctx, rowbuf, err)
                         : pcx_write(f, width, height, palette8, get_row, ctx, rowbuf, err);
    if (ferror(f) && !rc) {
        sprintf(err, "write error on %.150s (disk full?)", path);
        rc = 1;
    }
    if (fclose(f) && !rc) {
        sprintf(err, "write error on %.150s", path);
        rc = 1;
    }
    return rc;
}
