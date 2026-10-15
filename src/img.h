/* img.h - 8-bit palettized BMP and PCX, read and write, one row at a time. */
#ifndef IMG_H
#define IMG_H

#include <stdio.h>
#include "van.h"

typedef struct {
    FILE *f;
    int type;                  /* IMG_BMP or IMG_PCX */
    u32 width, height;
    unsigned char palette[768];   /* 8-bit RGB */
    /* BMP */
    long bits_offset, pos;     /* pos: current file position, -1 if unknown */
    u32 stride;
    int top_down;
    /* PCX decoder state (rows must be read in order 0..h-1) */
    u32 bytes_per_line, next_row;
    unsigned run_count;
    unsigned char run_value;
} ImgReader;

#define IMG_BMP 1
#define IMG_PCX 2

/* Type from the file name extension (.bmp / .pcx), or 0. */
int img_type_from_name(const char *path);

int  img_open(ImgReader *r, const char *path, char *err);
int  img_read_row(ImgReader *r, u32 y, unsigned char *row, char *err);   /* y from the top */
/* 1 if rows are stored bottom-up (read them from y = height-1 down for sequential access). */
int  img_bottom_up(const ImgReader *r);
void img_close(ImgReader *r);

/* Writers take a callback that fills row y (from the top) with width palette indexes. */
typedef int (*img_row_fn)(void *ctx, u32 y, unsigned char *row, char *err);
int img_write(const char *path, int type, u32 width, u32 height, const unsigned char *palette8,
              img_row_fn get_row, void *ctx, unsigned char *rowbuf, char *err);

#endif
