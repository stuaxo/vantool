/* play.h - platform interface for vanplay.  Implementations: play_dos.c (VGA/VESA), play_sdl.c (SDL2). */
#ifndef PLAY_H
#define PLAY_H

#include "van.h"

typedef struct {
    u32 force_mode;          /* DOS: video mode to use instead of the header's (0 = auto) */
    int scale;               /* SDL: window scale (0 = auto: largest that fits 1280x1024) */
    int fullscreen;          /* SDL */
    const char *capture;     /* SDL: on exit, save the last presented frame (as rendered) to this BMP */
    const char *format;      /* what is playing, for the window title (NULL: from the VanFile) */
} PlayOpts;

enum { KEY_NONE = -1, KEY_QUIT = 1, KEY_PAUSE, KEY_LEFT, KEY_RIGHT, KEY_FASTER, KEY_SLOWER, KEY_HOME };

int  plat_init(VanFile *v, const PlayOpts *o, char *err);   /* set the display up, load the palette */
void plat_row(void *ctx, u32 y, const unsigned char *row);  /* draw image row y (a van_row_cb) */
void plat_palette(const unsigned char *pal6);               /* new palette (768 bytes, 6-bit); shown with the next frame */
void plat_frame_done(u32 frame);                            /* present the frame just drawn */
int  plat_key(void);                                        /* KEY_* or KEY_NONE, never blocks */
u32  plat_ms(void);                                         /* monotonic milliseconds */
void plat_sleep(u32 ms);
void plat_done(void);                                       /* restore the display */
const char *plat_info(void);                                /* how the display was set up (-v) */
/* 1 if this display can show the VistaPro mode, 0 if not, -1 if modes do not apply (windowed). */
int  plat_mode_available(u32 mode, u32 width, u32 height);
int  plat_draws_visibly(void);   /* 1 if rows appear on screen as they are drawn (no hidden page/buffer) */
/* Draw a whole frame held in the store (store.h) at off, top row first, rows packed.  0 = done;
 * nonzero = not supported here, caller draws it row by row instead. */
int  plat_draw_store(u32 off);

#endif
