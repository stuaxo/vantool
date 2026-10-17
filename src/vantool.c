/* vantool - VistaPro VANIM (.VAN) utility for DOS and Linux.  See docs/VAN_SPEC.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "van.h"
#include "img.h"
#include "flc.h"
#include "sys.h"
#include "version.h"


static unsigned char rowbuf[VAN_MAX_WIDTH + 2];
static char err[256];

static void usage(void)
{
    static const char *const head[] = {
        "vantool " VERSION " - VistaPro VANIM (.VAN) animation utility",
        "",
        "  vantool info FILE.VAN",
        "  vantool extract FILE.VAN PREFIX [-t bmp|pcx] [-f FIRST] [-n COUNT]",
        "      write frames as PREFIXnnnn.BMP/.PCX",
        NULL
    };
    static const char *const tail[] = {
        "  vantool make OUT.VAN [-m MODE] [-pc] [-k] IMAGE...",
        "  vantool make OUT.VAN [-m MODE] [-pc] [-k] -p PATTERN -n COUNT [-s START]",
        "      build from 8-bit BMP/PCX frames; PATTERN is printf-style, e.g. FR%04d.PCX",
        "      -m MODE  video mode (default: from size, e.g. 0x13 or 0x101)",
        "      -pc      write VANIMPC (top-down) instead of VANIMWN (VP3 3.20 default)",
        "      -k       keep going if a frame's palette differs (use the first frame's)",
        "  vantool slice IN.VAN OUT.VAN FIRST [COUNT]",
        "  vantool flc IN.VAN OUT.FLC [-r FPS] [-f FIRST] [-n COUNT]",
        "      convert to an FLC animation; an OUT name ending .FLI writes the",
        "      older FLI format instead (320x200 only).  -r: playback speed (default 10)",
        "  vantool repair FILE.VAN [-pc]",
        "      rewrite the magic in place (fixes VP3 'continue' on a VANIMPC file)",
        NULL
    };
    int i;
    for (i = 0; head[i]; i++)
        puts(head[i]);
    if (*sys_extract_note())
        printf("      %s\n", sys_extract_note());     /* only where this system needs it */
    for (i = 0; tail[i]; i++)
        puts(tail[i]);
}

static int fail(void)
{
    fprintf(stderr, "vantool: %s\n", err);
    return 1;
}

static u32 num(const char *s)
{
    return (u32)strtoul(s, NULL, 0);
}

/* ---------- info ---------- */

static int cmd_info(int argc, char **argv)
{
    static VanFile v;
    int i, nonzero = 0, exact;
    unsigned char maxpal = 0;

    if (argc != 1) { usage(); return 2; }
    if (van_open(&v, argv[0], err))
        return fail();
    for (i = 0x28; i < VAN_HEADER_SIZE; i++)
        nonzero |= v.header[i];
    for (i = 0; i < VAN_PALETTE_SIZE; i++)
        if (v.palette[i] > maxpal) maxpal = v.palette[i];
    printf("file          %s\n", argv[0]);
    printf("variant       VANIM%s (%s rows)\n", v.pc ? "PC" : "WN", v.pc ? "top-down" : "bottom-up");
    printf("size          %lu x %lu\n", v.width, v.height);
    printf("frame_size    0x%lx (%lu)\n", v.frame_size, v.frame_size);
    printf("data_offset   0x%lx\n", v.data_offset);
    printf("frames        %lu", v.frames);
    if (v.trailing)
        printf("  (+%lu trailing bytes: partial frame ignored)", v.trailing);
    printf("\nimage_bytes   0x%lx\n", v.image_bytes);
    printf("colors        %lu\n", v.colors);
    printf("unk20         0x%lx\n", v.unk20);
    printf("video_mode    0x%lx", v.video_mode);
    if (van_pick_mode(v.width, v.height, &exact) == v.video_mode && exact)
        printf("  (matches size)");
    printf("\nheader 0x28.. %s\n", nonzero ? "NONZERO" : "all zero");
    printf("palette max   %u%s\n", maxpal, maxpal > 63 ? "  (NOT 6-bit!)" : " (6-bit VGA)");
    if (v.data_offset != VAN_DATA_OFFSET)
        printf("warning: data_offset is not 0x400; VIEWER.EXE will misread this file\n");
    van_close(&v);
    return 0;
}

/* ---------- extract ---------- */

typedef struct { VanFile *v; u32 frame; } FrameCtx;

static int frame_row(void *ctx, u32 y, unsigned char *row, char *e)
{
    FrameCtx *c = (FrameCtx *)ctx;
    return van_read_row(c->v, c->frame, y, row, e);
}

static int cmd_extract(int argc, char **argv)
{
    static VanFile v;
    static FrameCtx ctx;
    static unsigned char pal8[768];
    static char name[260];
    int type = IMG_BMP, i;
    u32 first = 0, count = 0, f;

    if (argc < 2) { usage(); return 2; }
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            i++;
            type = !strcmp(argv[i], "pcx") || !strcmp(argv[i], "PCX") ? IMG_PCX : IMG_BMP;
        } else if (!strcmp(argv[i], "-f") && i + 1 < argc) first = num(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) count = num(argv[++i]);
        else { usage(); return 2; }
    }
    if (strlen(argv[1]) > 240) { sprintf(err, "prefix too long"); return fail(); }
    if (van_open(&v, argv[0], err))
        return fail();
    /* full-range 6->8 bit: (v<<2)|(v>>4); >>2 gives the stored value back exactly */
    for (i = 0; i < 768; i++)
        pal8[i] = (unsigned char)((v.palette[i] << 2) | (v.palette[i] >> 4));
    if (!count || first + count > v.frames)
        count = first < v.frames ? v.frames - first : 0;
    ctx.v = &v;
    for (f = first; f < first + count; f++) {
        sprintf(name, "%s%04lu.%s", argv[1], f, type == IMG_PCX ? "PCX" : "BMP");
        ctx.frame = f;
        if (img_write(name, type, v.width, v.height, pal8, frame_row, &ctx, rowbuf, err)) {
            van_close(&v);
            return fail();
        }
        printf("%s\n", name);
    }
    van_close(&v);
    return 0;
}

/* ---------- make ---------- */

static int cmd_make(int argc, char **argv)
{
    const char *out, *pattern = NULL;
    char **files = NULL;
    int nfiles = 0, pc = 0, keep = 0, i, exact, rounded = 0, created = 0, up;
    static char err2[256];
    u32 mode = 0, count = 0, start = 0, n, total, y, k;
    int have_mode = 0;
    static VanWriter w;
    static ImgReader r;
    static unsigned char pal6[768], p6[768];
    static char name[260];

    if (argc < 2) { usage(); return 2; }
    out = argv[0];
    files = (char **)malloc(sizeof(char *) * (size_t)argc);
    if (!files) { sprintf(err, "out of memory"); return fail(); }
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) { mode = num(argv[++i]); have_mode = 1; }
        else if (!strcmp(argv[i], "-pc")) pc = 1;
        else if (!strcmp(argv[i], "-k")) keep = 1;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) pattern = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) count = num(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) start = num(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1]) { usage(); free(files); return 2; }
        else files[nfiles++] = argv[i];
    }
    if (pattern && (nfiles || !count)) { usage(); free(files); return 2; }
    total = pattern ? count : (u32)nfiles;
    if (!total) { usage(); free(files); return 2; }

    for (n = 0; n < total; n++) {
        const char *path = files[n];
        if (pattern) {
            if (strlen(pattern) > 200) { sprintf(err, "pattern too long"); goto bad; }
            sprintf(name, pattern, (int)(start + n));
            path = name;
        }
        if (img_open(&r, path, err))
            goto bad;
        for (i = 0; i < 768; i++) {
            p6[i] = (unsigned char)(r.palette[i] >> 2);
            if ((unsigned char)((p6[i] << 2) | (p6[i] >> 4)) != r.palette[i] && (p6[i] << 2) != r.palette[i])
                rounded = 1;
        }
        if (n == 0) {
            memcpy(pal6, p6, 768);
            if (!have_mode) {
                mode = van_pick_mode(r.width, r.height, &exact);
                if (!mode)
                    printf("note: %lux%lu is larger than any VP3 mode; video_mode set to 0\n", r.width, r.height);
                else if (!exact)
                    printf("note: %lux%lu is not a VP3 mode; using 0x%lx (VIEWER centres it)\n",
                           r.width, r.height, mode);
            }
            if (van_create(&w, out, r.width, r.height, pal6, mode, pc, err)) {
                img_close(&r);
                goto bad;
            }
            created = 1;
        } else {
            if (r.width != w.width || r.height != w.height) {
                sprintf(err, "%.150s: %lux%lu differs from first frame %lux%lu", path, r.width, r.height,
                        w.width, w.height);
                goto bad_img;
            }
            if (memcmp(p6, pal6, 768) != 0) {
                if (!keep) {
                    sprintf(err, "%.150s: palette differs from first frame (use -k to ignore)", path);
                    goto bad_img;
                }
                printf("warning: %s: palette differs; using first frame's\n", path);
            }
        }
        /* Visit rows in the image's storage order so the reads are sequential.  A bottom-up BMP
         * into a VANIMWN file (the usual case) is then sequential on both sides. */
        up = img_bottom_up(&r);
        for (k = 0; k < r.height; k++) {
            y = up ? r.height - 1 - k : k;
            if (img_read_row(&r, y, rowbuf, err) || van_write_row(&w, y, rowbuf, err))
                goto bad_img;
        }
        van_end_frame(&w);
        img_close(&r);
    }
    if (rounded)
        printf("note: palette rounded to 6-bit VGA levels\n");
    free(files);
    if (van_close_writer(&w, err))
        return fail();
    printf("%s: %lux%lu, %lu frames, mode 0x%lx, VANIM%s\n", out, w.width, w.height, w.frames, mode,
           pc ? "PC" : "WN");
    return 0;
bad_img:
    img_close(&r);
bad:
    if (created)
        van_close_writer(&w, err2);
    free(files);
    return fail();
}

/* ---------- slice ---------- */

static int cmd_slice(int argc, char **argv)
{
    static VanFile v;
    static VanWriter w;
    u32 first, count, f, y, k;

    if (argc < 3 || argc > 4) { usage(); return 2; }
    if (van_open(&v, argv[0], err))
        return fail();
    first = num(argv[2]);
    count = argc == 4 ? num(argv[3]) : 0;
    if (first > v.frames)
        first = v.frames;
    if (!count || first + count > v.frames)
        count = v.frames - first;
    if (van_create(&w, argv[1], v.width, v.height, v.palette, v.video_mode, v.pc, err)) {
        van_close(&v);
        return fail();
    }
    for (f = first; f < first + count; f++) {
        for (k = 0; k < v.height; k++) {
            y = v.pc ? k : v.height - 1 - k;     /* storage order: sequential read and write */
            if (van_read_row(&v, f, y, rowbuf, err) || van_write_row(&w, y, rowbuf, err)) {
                van_close(&v);
                van_close_writer(&w, err);
                return fail();
            }
        }
        van_end_frame(&w);
    }
    van_close(&v);
    if (van_close_writer(&w, err))
        return fail();
    printf("%s: %lu frames\n", argv[1], count);
    return 0;
}

/* ---------- flc ---------- */

/* Two readers, so the previous and the current frame are each read in order, not alternately. */
typedef struct {
    VanFile v[2];
    u32 at[2];      /* frame each reader is on */
    int last;       /* reader used last */
    u32 first;      /* source frame of output frame 0 */
} FlcSource;

static int flc_row(void *p, u32 frame, u32 y, unsigned char *row, char *e)
{
    FlcSource *s = (FlcSource *)p;
    int i = s->at[0] == frame ? 0 : s->at[1] == frame ? 1 : !s->last;
    s->at[i] = frame;
    s->last = i;
    return van_read_row(&s->v[i], s->first + frame, y, row, e);
}

static int cmd_flc(int argc, char **argv)
{
    static FlcSource s;
    const char *ext;
    u32 fps = 10, first = 0, count = 0;
    int i, fli, failed;

    if (argc < 2) { usage(); return 2; }
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) fps = num(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) first = num(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) count = num(argv[++i]);
        else { usage(); return 2; }
    }
    ext = strrchr(argv[1], '.');
    fli = ext && (!strcmp(ext, ".fli") || !strcmp(ext, ".FLI") || !strcmp(ext, ".Fli"));
    if (van_open(&s.v[0], argv[0], err))
        return fail();
    if (van_open(&s.v[1], argv[0], err)) {
        van_close(&s.v[0]);
        return fail();
    }
    s.at[0] = s.at[1] = (u32)-1;
    s.last = 1;
    if (fli && (s.v[0].width != 320 || s.v[0].height != 200)) {
        sprintf(err, "FLI files are always 320x200, this is %lux%lu: write a .FLC instead",
                s.v[0].width, s.v[0].height);
        failed = 1;
    } else {
        if (!count || first + count > s.v[0].frames)
            count = first < s.v[0].frames ? s.v[0].frames - first : 0;
        s.first = first;
        failed = flc_write(argv[1], fli, s.v[0].width, s.v[0].height, count, fps, s.v[0].palette,
                           flc_row, &s, err);
    }
    van_close(&s.v[0]);
    van_close(&s.v[1]);
    if (failed)
        return fail();
    printf("%s: %s, %lux%lu, %lu frames at %lu fps\n", argv[1], fli ? "FLI" : "FLC",
           s.v[0].width, s.v[0].height, count, fps ? fps : 1);
    return 0;
}

/* ---------- repair ---------- */

static int cmd_repair(int argc, char **argv)
{
    static VanFile v;
    FILE *f;
    int pc = argc == 2 && !strcmp(argv[1], "-pc");

    if (argc < 1 || argc > 2 || (argc == 2 && !pc)) { usage(); return 2; }
    if (van_open(&v, argv[0], err))
        return fail();
    van_close(&v);
    f = fopen(argv[0], "r+b");
    if (!f) { sprintf(err, "cannot open %.150s for writing", argv[0]); return fail(); }
    fwrite(pc ? "VANIMPC" : "VANIMWN", 1, 8, f);       /* includes the NUL */
    if (fclose(f)) { sprintf(err, "write error on %.150s", argv[0]); return fail(); }
    printf("%s: VANIM%s -> VANIM%s\n", argv[0], v.pc ? "PC" : "WN", pc ? "PC" : "WN");
    return 0;
}

int main(int argc, char **argv)
{
    sys_init(NULL);
    if (argc < 2 || !strcmp(argv[1], "/?") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage();
        return argc < 2 ? 2 : 0;
    }
    if (!strcmp(argv[1], "info"))    return cmd_info(argc - 2, argv + 2);
    if (!strcmp(argv[1], "extract")) return cmd_extract(argc - 2, argv + 2);
    if (!strcmp(argv[1], "make"))    return cmd_make(argc - 2, argv + 2);
    if (!strcmp(argv[1], "slice"))   return cmd_slice(argc - 2, argv + 2);
    if (!strcmp(argv[1], "repair"))  return cmd_repair(argc - 2, argv + 2);
    if (!strcmp(argv[1], "flc"))     return cmd_flc(argc - 2, argv + 2);
    usage();
    return 2;
}
