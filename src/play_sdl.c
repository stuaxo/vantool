/* play_sdl.c - vanplay display backend for Linux (SDL2). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <SDL.h>
#include "play.h"

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static Uint32 *pixels, pal32[256];
static u32 width, height, frames;
static const char *capture;
static char title[300];

int plat_init(VanFile *v, const PlayOpts *o, char *err)
{
    int scale = o->scale;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        sprintf(err, "SDL_Init: %.200s", SDL_GetError());
        return 1;
    }
    width = v->width;
    height = v->height;
    frames = v->frames;
    capture = o->capture;
    if (scale < 1) {
        for (scale = 1; width * (scale + 1) <= 1280 && height * (scale + 1) <= 1024; scale++)
            ;
    }
    plat_palette(v->palette);
    pixels = (Uint32 *)calloc((size_t)width * height, sizeof(Uint32));
    win = SDL_CreateWindow("vanplay", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           (int)width * scale, (int)height * scale,
                           o->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (!pixels || !win) {
        sprintf(err, "cannot create window: %.200s", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) {
        sprintf(err, "SDL_CreateRenderer: %.200s", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(ren, (int)width, (int)height);
    SDL_RenderSetIntegerScale(ren, SDL_TRUE);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, (int)width, (int)height);
    if (!tex) {
        sprintf(err, "SDL_CreateTexture: %.200s", SDL_GetError());
        return 1;
    }
    sprintf(title, "vanplay - %.200s", o->format ? o->format : v->pc ? "VANIMPC" : "VANIMWN");
    return 0;
}

void plat_palette(const unsigned char *p)
{
    int i;
    /* 6-bit VGA DAC levels shown as (v<<2)|(v>>4), exactly what VGA hardware displays.  Rows already
     * drawn keep their colours, so a player that changes the palette redraws the whole frame. */
    for (i = 0; i < 256; i++, p += 3)
        pal32[i] = 0xFF000000UL | ((Uint32)((p[0] << 2) | (p[0] >> 4)) << 16) |
                   ((Uint32)((p[1] << 2) | (p[1] >> 4)) << 8) | (Uint32)((p[2] << 2) | (p[2] >> 4));
}

void plat_row(void *ctx, u32 y, const unsigned char *row)
{
    Uint32 *d = pixels + (size_t)y * width;
    u32 x;
    (void)ctx;
    for (x = 0; x < width; x++)
        d[x] = pal32[row[x]];
}

static void render(void)
{
    SDL_UpdateTexture(tex, NULL, pixels, (int)(width * sizeof(Uint32)));
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
}

void plat_frame_done(u32 frame)
{
    char t[340];
    render();
    SDL_RenderPresent(ren);
    sprintf(t, "%s  frame %lu/%lu", title, frame + 1, frames);
    SDL_SetWindowTitle(win, t);
}

int plat_key(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT)
            return KEY_QUIT;
        if (e.type != SDL_KEYDOWN)
            continue;
        switch (e.key.keysym.sym) {
        case SDLK_ESCAPE: case SDLK_q: return KEY_QUIT;
        case SDLK_SPACE: return KEY_PAUSE;
        case SDLK_LEFT: return KEY_LEFT;
        case SDLK_RIGHT: return KEY_RIGHT;
        case SDLK_PLUS: case SDLK_EQUALS: case SDLK_KP_PLUS: return KEY_FASTER;
        case SDLK_MINUS: case SDLK_KP_MINUS: return KEY_SLOWER;
        case SDLK_HOME: return KEY_HOME;
        default: break;
        }
    }
    return KEY_NONE;
}

const char *const *plat_help(void)
{
    static const char *const text[] = {
        "  -x SCALE   window scale (default: fit 1280x1024)",
        "  -f         fullscreen",
        "  -c OUT.BMP on exit, save the last frame shown, as rendered (for testing)",
        NULL
    };
    return text;
}

const char *plat_info(void)
{
    static char s[128];
    sprintf(s, "SDL2 %.40s renderer, %lux%lu", ren ? "accelerated or software" : "no", width, height);
    return s;
}

int plat_mode_available(u32 mode, u32 width, u32 height)
{
    (void)mode; (void)width; (void)height;
    return -1;                    /* any size is shown in a window */
}

int plat_draw_store(u32 off)
{
    (void)off;
    return 1;                     /* the generic row path is already a plain memcpy here */
}

int plat_draws_visibly(void)
{
    return 0;                     /* the window only changes at SDL_RenderPresent */
}

u32 plat_ms(void)
{
    return SDL_GetTicks();
}

void plat_sleep(u32 ms)
{
    SDL_Delay(ms);
}

void plat_done(void)
{
    if (capture && ren) {
        /* Re-render the last frame and read back what the renderer actually produced. */
        int w, h;
        SDL_Surface *s;
        render();
        SDL_GetRendererOutputSize(ren, &w, &h);
        s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (s && SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0)
            SDL_SaveBMP(s, capture);
        else
            fprintf(stderr, "vanplay: capture failed: %s\n", SDL_GetError());
        SDL_FreeSurface(s);
    }
    if (!win && !ren)
        return;                       /* already done (cleanup may run twice) */
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    tex = NULL;
    ren = NULL;
    win = NULL;
    free(pixels);
    pixels = NULL;
    SDL_Quit();
}
