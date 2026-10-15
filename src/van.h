/* van.h - VistaPro VANIM (.VAN) reader/writer.  See docs/VAN_SPEC.md.
 *
 * Portable C89: builds with gcc (Linux) and Open Watcom (16-bit DOS, large model).
 * Frames are handled one row at a time so no buffer exceeds one image row.
 */
#ifndef VAN_H
#define VAN_H

#include <stdio.h>

#define VAN_HEADER_SIZE  256
#define VAN_PALETTE_SIZE 768
#define VAN_DATA_OFFSET  0x400L
#define VAN_MAX_WIDTH    4096
#define VAN_IOBUF        16384   /* stdio buffer per open file: fewer, larger DOS reads/writes */
#define VAN_CACHE        61440u  /* preferred I/O block; smaller if memory is short (min 4 KB, >= 1 row) */

typedef unsigned long u32;

typedef struct {
    FILE *f;
    int pc;                 /* 1 = VANIMPC (top-down rows), 0 = VANIMWN (bottom-up) */
    u32 image_bytes, data_offset, frame_size, colors;
    u32 width, height, unk20, video_mode;
    u32 frames;             /* whole frames present */
    u32 trailing;           /* bytes of a trailing partial frame */
    unsigned char *cache;   /* csize bytes; holds the file from cstart, clen bytes valid */
    unsigned csize;
    long cstart, last;      /* last: position of the previous row read (-1 none) */
    unsigned clen;
    unsigned char header[VAN_HEADER_SIZE];
    unsigned char palette[VAN_PALETTE_SIZE];   /* 6-bit values as stored */
} VanFile;

typedef struct {
    FILE *f;
    int pc;
    u32 width, height, frames;
    unsigned char *cache;   /* block being assembled: file bytes [cstart, cstart+csize) */
    unsigned csize;
    long cstart, last;
    long dlo, dhi;          /* dirty (not yet written) range, dlo == dhi when clean */
    int cvalid;
} VanWriter;

/* All functions return 0 on success, nonzero on error with a message in err (>= 128 bytes). */
int  van_open(VanFile *v, const char *path, char *err);
int  van_read_row(VanFile *v, u32 frame, u32 y, unsigned char *row, char *err);  /* y counts from the top */
void van_close(VanFile *v);
/* Read a whole frame sequentially (no per-row seeks), calling cb with each row's position from the top. */
typedef void (*van_row_cb)(void *ctx, u32 y, const unsigned char *row);
int  van_read_frame(VanFile *v, u32 frame, unsigned char *rowbuf, van_row_cb cb, void *ctx, char *err);

int  van_create(VanWriter *w, const char *path, u32 width, u32 height,
                const unsigned char *palette6, u32 video_mode, int pc, char *err);
/* Write row y (from the top) of the frame currently being written; rows may come in any order. */
int  van_write_row(VanWriter *w, u32 y, const unsigned char *row, char *err);
void van_end_frame(VanWriter *w);
int  van_close_writer(VanWriter *w, char *err);

/* VP3 3.20 video modes (spec 9.4). */
typedef struct { u32 mode, width, height; } VanMode;
extern const VanMode van_modes[];
extern const int van_mode_count;
/* Exact mode for the size, else smallest that fits; returns 0 if none.  *exact set to 1/0. */
u32 van_pick_mode(u32 width, u32 height, int *exact);

/* Allocate an I/O block: VAN_CACHE bytes if possible, else smaller (never below min), keeping 4 KB of
 * heap free when it can; NULL if even min is unavailable. */
unsigned char *van_alloc_block(unsigned min, unsigned *size);

u32  van_get32(const unsigned char *p);
void van_put32(unsigned char *p, u32 v);

#endif
