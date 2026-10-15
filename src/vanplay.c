/* vanplay - play VistaPro VANIM (.VAN) animations.  DOS: VGA/VESA.  Linux: SDL2. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "van.h"
#include "play.h"
#include "sys.h"
#include "ring.h"
#include "store.h"

#define VERSION "0.1"

static unsigned char rowbuf[VAN_MAX_WIDTH + 2];
static char err[256], info[320];

/* Draw a frame: from the read-ahead ring when it has one, else straight from disk. */
static int show(VanFile *v, u32 frame)
{
    if (ring_slots()) {
        ring_want_from(frame);
        if (!ring_has(frame) && plat_draws_visibly()) {
            /* Drawing straight onto the visible screen (e.g. 1280x1024 on a 2 MB card): finish
             * loading the frame into memory first, so it appears in one fast copy instead of
             * painting in at disk speed. */
            int r;
            while (!ring_has(frame) && (r = ring_step(err)) > 0)
                ;
            if (r < 0)
                return 1;
        }
        if (ring_has(frame)) {                                         /* from memory: */
            if (plat_draw_store((u32)ring_offset(frame)) &&            /* straight to video, or */
                ring_draw(frame, plat_row, NULL, err))                 /* row by row */
                return 1;
        } else if (ring_stream(frame, plat_row, NULL, err)) {          /* late: disk, copy kept */
            return 1;
        }
    } else if (van_read_frame(v, frame, rowbuf, plat_row, NULL, err)) {
        return 1;
    }
    plat_frame_done(frame);
    if (ring_slots())
        ring_want_from(frame + 1);            /* read ahead from the next frame on */
    return 0;
}

static void cleanup(void)
{
    plat_done();
    ring_done();                              /* frees XMS, which DOS would otherwise lose */
}

static void null_row(void *ctx, u32 y, const unsigned char *row)
{
    (void)ctx; (void)y; (void)row;
}

static void report(const char *what, VanFile *v, u32 f, u32 ms)
{
    if (!ms) ms = 1;
    printf("%s: %lu frames %lux%lu in %lu ms = %lu.%02lu fps\n", what, f, v->width, v->height, ms,
           f * 1000UL / ms, (f * 100000UL / ms) % 100);
}

/* -t: show frames as fast as possible and report the rate.
 * what 0 = read + draw, 1 = read only (disk cost), 2 = draw only (display cost),
 *      3 = through the read-ahead ring, two passes (the second pass shows the effect of caching). */
static int benchmark(VanFile *v, int what, u32 count)
{
    u32 f, y, t0, ms[2], pass;
    if (what == 3) {
        for (pass = 0; pass < 2; pass++) {
            t0 = plat_ms();
            for (f = 0; f < count && !show(v, f % v->frames); f++)
                ;
            ms[pass] = plat_ms() - t0;
        }
        cleanup();
        report("ring pass 1", v, count, ms[0]);
        report("ring pass 2", v, count, ms[1]);
        return 0;
    }
    t0 = plat_ms();
    for (f = 0; f < count; f++) {
        if (what == 2) {
            for (y = 0; y < v->height; y++)
                plat_row(NULL, y, rowbuf);
        } else if (van_read_frame(v, f % v->frames, rowbuf, what == 1 ? null_row : plat_row, NULL, err)) {
            break;
        }
        plat_frame_done(f % v->frames);
        if (plat_key() == KEY_QUIT)
            break;
    }
    ms[0] = plat_ms() - t0;
    cleanup();
    report(what == 0 ? "read+draw" : what == 1 ? "read only" : "draw only", v, f, ms[0]);
    return 0;
}

static void usage(void)
{
    static const char *const text[] = {
        "vanplay " VERSION " - VistaPro VANIM (.VAN) player",
        "",
        "  vanplay FILE.VAN [-r FPS] [-l LOOPS] [-p] [-s FRAME] [-n COUNT] [-m MODE] [-x SCALE] [-f] [-c OUT.BMP]",
        "",
        "  -r FPS     frames per second (default 10)",
        "  -l LOOPS   play this many times, 0 = forever (default 0)",
        "  -p         start paused",
        "  -s FRAME   start at this frame",
        "  -n COUNT   exit after showing COUNT frames (for testing)",
        "  -t WHAT    benchmark: show -n frames (default all) flat out, print fps;",
        "             WHAT 0 = read+draw, 1 = read only, 2 = draw only, 3 = via read-ahead (2 passes)",
        "  -b KB      read-ahead memory (default 8192; DOS uses XMS); 0 = stream from disk",
        "  -v         print display and buffering details on exit",
        "  -m MODE    DOS: force a video mode, e.g. 0x13 or 0x101 (default: the file's)",
        "  -x SCALE   Linux: window scale (default: fit 1280x1024)",
        "  -f         Linux: fullscreen",
        "  -c OUT.BMP Linux: on exit, save the last frame shown, as rendered (for testing)",
        "",
        "  keys: Esc/q quit, Space pause, Left/Right step, +/- speed, Home first frame",
        NULL
    };
    int i;
    for (i = 0; text[i]; i++)
        puts(text[i]);
}

int main(int argc, char **argv)
{
    static VanFile v;
    PlayOpts o;
    const char *path = NULL;
    u32 fps = 10, loops = 0, frame = 0, shown = 0, limit = 0, done_loops = 0, next, budget_kb = 8192;
    u32 t_start, late = 0;
    int paused = 0, i, k, redraw = 1, bench = -1, verbose = 0;

    memset(&o, 0, sizeof(o));
    sys_init(cleanup);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "/?") || !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        }
        if (!strcmp(argv[i], "-r") && i + 1 < argc) fps = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) loops = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-p")) paused = 1;
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) frame = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) limit = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) bench = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) budget_kb = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) o.force_mode = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) o.scale = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-f")) o.fullscreen = 1;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) o.capture = argv[++i];
        else if (argv[i][0] == '-') { usage(); return 2; }
        else path = argv[i];
    }
    if (!path) { usage(); return 2; }
    if (fps < 1) fps = 1;
    if (van_open(&v, path, err)) {
        fprintf(stderr, "vanplay: %s\n", err);
        return 1;
    }
    if (v.frames == 0) {
        fprintf(stderr, "vanplay: %s has no frames\n", path);
        return 1;
    }
    if (frame >= v.frames)
        frame = v.frames - 1;
    if (ring_init(&v, budget_kb * 1024UL, err)) {
        fprintf(stderr, "vanplay: %s\n", err);
        van_close(&v);
        return 1;
    }
    if (plat_init(&v, &o, err)) {
        ring_done();
        fprintf(stderr, "vanplay: %s\n", err);
        van_close(&v);
        return 1;
    }

    sprintf(info, "%.130s; read-ahead %lu of %lu frames in %s%s%.40s; I/O block %u bytes", plat_info(),
            ring_slots(), v.frames, ring_slots() ? store_kind() : "none", *ring_why() ? " - " : "",
            ring_why(), v.csize);
    if (bench >= 0) {
        int rc = benchmark(&v, bench, limit ? limit : v.frames);
        if (verbose)
            puts(info);
        van_close(&v);
        return rc;
    }
    next = t_start = plat_ms();
    for (;;) {
        if (redraw) {
            if (!paused && shown && (long)(plat_ms() - next) > (long)(1000 / fps))
                late++;                           /* more than a frame behind schedule */
            if (show(&v, frame))
                break;
            redraw = 0;
            if (++shown >= limit && limit)
                break;
            next += 1000 / fps;
        }
        k = plat_key();
        if (k == KEY_QUIT) break;
        else if (k == KEY_PAUSE) { paused = !paused; next = plat_ms(); }
        else if (k == KEY_LEFT) { paused = 1; frame = frame ? frame - 1 : v.frames - 1; redraw = 1; }
        else if (k == KEY_RIGHT) { paused = 1; frame = (frame + 1) % v.frames; redraw = 1; }
        else if (k == KEY_HOME) { frame = 0; redraw = 1; next = plat_ms(); }
        else if (k == KEY_FASTER && fps < 60) fps++;
        else if (k == KEY_SLOWER && fps > 1) fps--;
        if (redraw)
            continue;
        if (paused || (long)(next - plat_ms()) > 0) {
            /* Waiting for the next frame: read ahead instead of idling (one chunk at a time, so keys
             * and timing stay responsive).  Sleep only when the ring is full. */
            int r = ring_slots() ? ring_step(err) : 0;
            if (r < 0)
                break;
            if (r == 0)
                plat_sleep(5);
            continue;
        }
        if (frame + 1 < v.frames)
            frame++;
        else {
            if (loops && ++done_loops >= loops)
                break;
            frame = 0;
        }
        redraw = 1;
        if ((long)(plat_ms() - next) > 1000)     /* fell far behind (slow disk): don't race to catch up */
            next = plat_ms();
    }
    t_start = plat_ms() - t_start;
    cleanup();
    van_close(&v);
    if (verbose) {
        puts(info);
        if (!t_start) t_start = 1;
        printf("played %lu frames in %lu ms = %lu.%01lu fps (asked %lu), %lu late\n", shown, t_start,
               shown * 1000UL / t_start, (shown * 10000UL / t_start) % 10, fps, late);
    }
    if (err[0]) {
        fprintf(stderr, "vanplay: %s\n", err);
        return 1;
    }
    return 0;
}
