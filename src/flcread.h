/* flcread.h - read FLI and FLC animations (Autodesk Animator) for the player.
 *
 * The picture is kept as one buffer per row, so no allocation is bigger than an image row (fits 16-bit
 * DOS).  Frames are deltas, so the reader holds the current picture and moves between frames by
 * decoding: the next frame is one step, anything else restarts from the first frame.  The file's
 * trailing "ring" frame, which takes the last frame back to the first, is used to loop.
 */
#ifndef FLCREAD_H
#define FLCREAD_H

#include "van.h"

typedef struct {
    FILE *f;
    int fli;                       /* 1 = FLI (320x200), 0 = FLC */
    int bad;                       /* ran off the end of the file while decoding */
    u32 width, height;
    u32 frames;                    /* not counting the ring frame */
    u32 delay_ms;                  /* per frame, from the header; 0 if the file does not say */
    unsigned char **rows;          /* height rows of width pixels: the current picture */
    unsigned char pal6[VAN_PALETTE_SIZE];   /* current palette, 6-bit levels */
    unsigned pal_gen;              /* changes whenever pal6 does */
    long flen, first_pos, second_pos;   /* file length; where the first and second frames start */
    long pos;                      /* the next frame to decode */
    long cur;                      /* frame now in rows, -1 before the first */
} FlcFile;

/* 1 if the file starts like an FLI or FLC, 0 if not (or unreadable). */
int  flcr_probe(const char *path);
/* All functions return 0 on success, nonzero on error with a message in err (>= 128 bytes). */
int  flcr_open(FlcFile *c, const char *path, char *err);
/* Make rows hold the picture of this frame (0 .. frames-1). */
int  flcr_seek(FlcFile *c, u32 frame, char *err);
void flcr_close(FlcFile *c);

#endif
