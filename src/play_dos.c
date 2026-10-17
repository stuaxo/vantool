/* play_dos.c - vanplay display backend for DOS: VGA mode 13h and VESA banked 256-colour modes.
 * 16-bit real mode (Open Watcom, large model).  Same display behaviour as VIEWER.EXE: switch to the
 * file's video mode, centre smaller images, load the 6-bit palette straight into the DAC.
 */
#include <stdio.h>
#include <string.h>
#include <i86.h>
#include <dos.h>
#include <conio.h>
#include <malloc.h>
#include "play.h"
#include "store.h"

static struct {
    u32 mode, xres, yres, gran;     /* gran: bank granularity in bytes */
    unsigned seg, bpl;
    int vesa;
    u32 ox, oy, width;
    int pages;                      /* VESA video pages in use: 1 direct, 2 double, 3 triple buffered */
    unsigned win;                   /* VESA window used for writing: 0 = A, 1 = B */
    unsigned wf_off, wf_seg;        /* VBE window function (0:0 = use INT 10h) */
    unsigned back;                  /* page being drawn */
    u32 page_bytes;
} scr;
static unsigned cur_bank = 0xFFFFu;
static unsigned char __far *backbuf;  /* mode 13h: whole frame, copied to A000 during vertical retrace */
static unsigned char info[256];      /* VESA ModeInfoBlock */
static unsigned char vbeinfo[512];   /* VESA VbeInfoBlock */
static unsigned vbe_version;         /* BCD, e.g. 0x0200; 0 = no VESA */
static unsigned char vbe_caps;       /* VbeInfoBlock Capabilities, low byte */
static char infostr[160];
static u32 refresh_us;               /* measured vertical refresh period */
static u32 t_flip[2];                /* time of the last flip, and of the one before it */
static int need_wait;                /* check the page is off screen before drawing into it */
static u32 vh;                       /* image height */

static const u32 mode_order[] = { 0x13, 0x100, 0x101, 0x103, 0x105, 0x107 };

/* Copy n bytes far-to-far with 32-bit moves (rep movsd, then 0-3 bytes).  On VLB/PCI video this
 * roughly doubles write bandwidth over 16-bit moves; it needs a 386, as the whole build does (-3). */
void fcopy32(void __far *dst, const void __far *src, unsigned n);
#pragma aux fcopy32 =   \
    "push ds"           \
    "mov ds, dx"        \
    "mov ax, cx"        \
    "shr cx, 2"         \
    "rep movsd"         \
    "mov cx, ax"        \
    "and cx, 3"         \
    "rep movsb"         \
    "pop ds"            \
    parm [es di] [dx si] [cx] \
    modify [ax cx si di];

/* Call the VBE window function directly (its far address is in the ModeInfoBlock), avoiding the
 * INT 10h dispatch on every bank switch.  BH = 0 (set), BL = window, DX = position in granules. */
static void bank_far(unsigned win, unsigned bank, unsigned off, unsigned seg);
#pragma aux bank_far =  \
    "push bp"           \
    "push cx"           \
    "push ax"           \
    "mov bp, sp"        \
    "call dword ptr ss:[bp]" \
    "add sp, 4"         \
    "pop bp"            \
    parm [bx] [dx] [ax] [cx] \
    modify [ax bx cx dx si di es];

static void int10(union REGS *r, struct SREGS *s)
{
    int86x(0x10, r, r, s);
}

/* Fill scr for mode if the hardware can show a w x h 8-bit image in it. */
static int probe(u32 mode, u32 w, u32 h)
{
    union REGS r;
    struct SREGS s;
    unsigned char far *p = (unsigned char far *)info;

    memset(&scr, 0, sizeof(scr));
    if (mode == 0x13) {
        if (w > 320 || h > 200) return 0;
        scr.mode = 0x13; scr.xres = 320; scr.yres = 200; scr.bpl = 320;
        scr.seg = 0xA000; scr.gran = 65536UL; scr.vesa = 0;
        return 1;
    }
    segread(&s);
    r.w.ax = 0x4F01;
    r.w.cx = (unsigned)mode;
    s.es = FP_SEG(p);
    r.w.di = FP_OFF(p);
    int10(&r, &s);
    if (r.w.ax != 0x004F) return 0;
    if (!(info[0] & 1) || info[0x19] != 8) return 0;            /* supported, 8 bpp */
    scr.mode = mode;
    scr.gran = (u32)(info[4] | (info[5] << 8)) * 1024UL;
    if ((info[2] & 5) == 5) {                                   /* window A exists and is writable */
        scr.win = 0;
        scr.seg = info[8] | (info[9] << 8);
    } else if ((info[3] & 5) == 5) {                            /* some cards write through window B */
        scr.win = 1;
        scr.seg = info[0x0A] | (info[0x0B] << 8);
    } else {
        return 0;
    }
    scr.wf_off = info[0x0C] | (info[0x0D] << 8);
    scr.wf_seg = info[0x0E] | (info[0x0F] << 8);
    scr.bpl = info[0x10] | (info[0x11] << 8);
    scr.xres = info[0x12] | (info[0x13] << 8);
    scr.yres = info[0x14] | (info[0x15] << 8);
    scr.vesa = 1;
    scr.pages = info[0x1D] >= 2 ? 3 : info[0x1D] + 1;   /* NumberOfImagePages is "pages - 1" */
    if (!scr.gran || !scr.seg || scr.xres < w || scr.yres < h) return 0;
    return 1;
}

/* Microseconds, from the BIOS tick count plus the 8254 timer's channel 0 count, without reprogramming
 * the timer.  The read-back command latches count and status together; the status gives the counter
 * mode (2: counts down by 1 per period; 3: square wave, counts down by 2 twice per period, and the OUT
 * bit says which half).  Only differences are used, so the 32-bit wrap (about 70 min) does not matter. */
static u32 now_us(void)
{
    volatile unsigned long __far *ticks = (volatile unsigned long __far *)MK_FP(0x40, 0x6C);
    unsigned long t1, t2;
    unsigned st, lo, hi;
    u32 count, pos;
    do {
        t1 = *ticks;
        _disable();
        outp(0x43, 0xC2);                         /* read-back: latch status and count, channel 0 */
        st = inp(0x40);
        lo = inp(0x40);
        hi = inp(0x40);
        _enable();
        t2 = *ticks;
    } while (t1 != t2);                           /* a tick happened meanwhile: read again */
    count = (u32)lo | ((u32)hi << 8);
    if (!count)
        count = 65536UL;
    if (((st >> 1) & 7) == 3)
        pos = (st & 0x80) ? (65536UL - count) / 2 : 32768UL + (65536UL - count) / 2;
    else
        pos = 65536UL - count;
    return t1 * 54925UL + pos * 838UL / 1000UL;
}

static void wait_retrace_start(void)
{
    while (inp(0x3DA) & 8)
        ;                                         /* end of any retrace in progress... */
    while (!(inp(0x3DA) & 8))
        ;                                         /* ...then the start of the next one */
}

static void measure_refresh(void)
{
    u32 t0, best = 0;
    int i;
    wait_retrace_start();
    for (i = 0; i < 4; i++) {
        t0 = now_us();
        wait_retrace_start();
        if (now_us() - t0 > best)
            best = now_us() - t0;
    }
    if (best < 8000UL || best > 30000UL)          /* implausible timer reading: be conservative */
        best = 20000UL;
    refresh_us = best + best / 16;                /* small safety margin */
}

static void query_vbe(void)
{
    union REGS r;
    struct SREGS s;
    unsigned char far *p = (unsigned char far *)vbeinfo;
    segread(&s);
    memcpy(vbeinfo, "VBE2", 4);                  /* ask for VBE 2.0+ information */
    r.w.ax = 0x4F00;
    s.es = FP_SEG(p);
    r.w.di = FP_OFF(p);
    int10(&r, &s);
    if (r.w.ax == 0x004F && !memcmp(vbeinfo, "VESA", 4)) {
        vbe_version = vbeinfo[4] | (vbeinfo[5] << 8);
        vbe_caps = vbeinfo[0x0A];
    }
}

/* 6-bit palette.  Straight to the DAC ports, unless VBE says the controller isn't VGA-compatible,
 * in which case VBE function 09h is the only correct way. */
static void set_palette(const unsigned char *pal6)
{
    int i;
    if (scr.vesa && (vbe_caps & 2)) {
        static unsigned char table[1024];        /* VBE order: blue, green, red, 0 */
        union REGS r;
        struct SREGS s;
        unsigned char far *p = (unsigned char far *)table;
        for (i = 0; i < 256; i++) {
            table[i * 4] = pal6[i * 3 + 2];
            table[i * 4 + 1] = pal6[i * 3 + 1];
            table[i * 4 + 2] = pal6[i * 3];
            table[i * 4 + 3] = 0;
        }
        segread(&s);
        r.w.ax = 0x4F09;
        r.h.bl = 0;
        r.w.cx = 256;
        r.w.dx = 0;
        s.es = FP_SEG(p);
        r.w.di = FP_OFF(p);
        int10(&r, &s);
        if (r.w.ax == 0x004F)
            return;
    }
    outp(0x3C8, 0);
    for (i = 0; i < VAN_PALETTE_SIZE; i++)
        outp(0x3C9, pal6[i]);
}

int plat_draws_visibly(void)
{
    return !backbuf && !(scr.vesa && scr.pages >= 2);
}

const char *plat_info(void)
{
    return infostr;
}

int plat_mode_available(u32 mode, u32 width, u32 height)
{
    return probe(mode, width, height);
}

int plat_init(VanFile *v, const PlayOpts *o, char *err)
{
    union REGS r;
    struct SREGS s;
    int i, ok = 0;

    query_vbe();

    if (o->force_mode)
        ok = probe(o->force_mode, v->width, v->height);
    else {
        ok = probe(v->video_mode, v->width, v->height);
        for (i = 0; !ok && i < (int)(sizeof(mode_order) / sizeof(mode_order[0])); i++)
            ok = probe(mode_order[i], v->width, v->height);
    }
    if (!ok) {
        sprintf(err, "no usable video mode for %lux%lu (wanted 0x%lx)", v->width, v->height,
                o->force_mode ? o->force_mode : v->video_mode);
        return 1;
    }
    segread(&s);
    if (scr.vesa) {
        r.w.ax = 0x4F02;
        r.w.bx = (unsigned)scr.mode;
    } else {
        r.w.ax = 0x0013;
    }
    int10(&r, &s);
    if (scr.vesa && r.w.ax != 0x004F) {
        sprintf(err, "cannot set video mode 0x%lx", scr.mode);
        return 1;
    }
    scr.width = v->width;
    vh = v->height;
    scr.ox = (scr.xres - v->width) / 2;
    scr.oy = (scr.yres - v->height) / 2;
    cur_bank = 0xFFFFu;
    scr.page_bytes = scr.yres * scr.bpl;
    scr.back = 0;
    if (scr.vesa && scr.pages >= 2) {
        r.w.ax = 0x4F07;                          /* can we move the display start? */
        r.w.bx = 0;
        r.w.cx = 0;
        r.w.dx = 0;
        int10(&r, &s);
        if (r.w.ax == 0x004F) {
            scr.back = 1;                         /* page 0 is shown; draw into page 1 */
            measure_refresh();
            t_flip[0] = t_flip[1] = now_us() - refresh_us;
            need_wait = 0;
        } else {
            scr.pages = 1;
        }
    }
    if (!scr.vesa) {
        backbuf = (unsigned char __far *)_fmalloc(64000u);
        if (backbuf)
            _fmemset(backbuf, 0, 64000u);
    }
    segread(&s);
    r.h.ah = 0x48;                                /* largest free DOS block (paragraphs) */
    r.w.bx = 0xFFFF;
    intdosx(&r, &r, &s);
    sprintf(infostr, "%u KB conventional free; mode 0x%lx %lux%lu, VBE %u.%u, %s, refresh %lu us",
            r.w.bx / 64, scr.mode, scr.xres, scr.yres, vbe_version >> 8, vbe_version & 0xFF,
            backbuf ? "mode 13h back buffer" : !scr.vesa || scr.pages < 2 ? "direct (1 page)" :
            scr.pages == 2 ? "double buffered" : "triple buffered", refresh_us);
    set_palette(v->palette);
    return 0;
}

static void set_bank(unsigned bank)
{
    union REGS r;
    struct SREGS s;
    if (bank == cur_bank) return;
    cur_bank = bank;
    if (scr.wf_seg || scr.wf_off) {
        bank_far(scr.win, bank, scr.wf_off, scr.wf_seg);
        return;
    }
    segread(&s);
    r.w.ax = 0x4F05;
    r.w.bx = scr.win;
    r.w.dx = bank;
    int10(&r, &s);
}

/* Before drawing into a page a flip took off screen, let at least one refresh pass since that flip. */
static void page_ready(void)
{
    if (need_wait) {
        u32 ref = scr.pages >= 3 ? t_flip[1] : t_flip[0];
        while (now_us() - ref < refresh_us)
            ;
        need_wait = 0;
    }
}

/* Copy len bytes from the store at src to video offset off (page included), one XMS move per bank
 * window piece: no CPU copy at all. */
static int store_to_vram(u32 src, u32 off, u32 len)
{
    while (len) {
        unsigned bank = (unsigned)(off / scr.gran);
        u32 woff = off - (u32)bank * scr.gran;
        u32 chunk = 65536UL - woff;
        if (chunk > 0xFFF0UL) chunk = 0xFFF0UL;    /* store moves take an unsigned length */
        if (chunk > len) chunk = len;
        if (scr.vesa) set_bank(bank);
        if (store_get(src, (unsigned char *)MK_FP(scr.seg, (unsigned)woff), (unsigned)chunk))
            return 1;
        src += chunk;
        off += chunk;
        len -= chunk;
    }
    return 0;
}

int plat_draw_store(u32 src)
{
    u32 y, page = scr.back * scr.page_bytes;
    if (backbuf) {                                  /* mode 13h: into the off-screen frame */
        if (scr.width == 320)
            return store_get(src, backbuf + (unsigned)(scr.oy * 320), (unsigned)(scr.width * vh));
        for (y = 0; y < vh; y++)
            if (store_get(src + y * scr.width, backbuf + (unsigned)((y + scr.oy) * 320 + scr.ox),
                          (unsigned)scr.width))
                return 1;
        return 0;
    }
    page_ready();
    if (scr.width == scr.bpl)                       /* full-width: the frame is one contiguous run */
        return store_to_vram(src, page + scr.oy * scr.bpl, scr.width * vh);
    for (y = 0; y < vh; y++)
        if (store_to_vram(src + y * scr.width, page + (y + scr.oy) * scr.bpl + scr.ox, scr.width))
            return 1;
    return 0;
}

void plat_row(void *ctx, u32 y, const unsigned char *row)
{
    u32 off = (y + scr.oy) * scr.bpl + scr.ox + scr.back * scr.page_bytes, len = scr.width;
    (void)ctx;
    if (backbuf) {
        fcopy32(backbuf + (unsigned)off, row, (unsigned)len);
        return;
    }
    /* About to draw into a page that a flip took off screen.  A flip takes effect at the next retrace,
     * so once a refresh period has passed since that flip the page is hidden.  With 3 pages that flip
     * was two frames ago, so this practically never waits. */
    page_ready();
    while (len) {
        unsigned bank = (unsigned)(off / scr.gran);
        u32 woff = off - (u32)bank * scr.gran;
        u32 chunk = 65536UL - woff;
        if (chunk > len) chunk = len;
        if (scr.vesa) set_bank(bank);
        fcopy32(MK_FP(scr.seg, (unsigned)woff), row, (unsigned)chunk);
        row += chunk;
        off += chunk;
        len -= chunk;
    }
}

/* A new palette is held back until its frame is shown, so the old picture never appears in the new colours. */
static unsigned char pending_pal[VAN_PALETTE_SIZE];
static int pal_pending;

void plat_palette(const unsigned char *pal6)
{
    memcpy(pending_pal, pal6, sizeof(pending_pal));
    pal_pending = 1;
}

void plat_frame_done(u32 frame)
{
    (void)frame;
    if (backbuf) {
        wait_retrace_start();                     /* mode 13h: copy the whole frame during retrace */
        fcopy32(MK_FP(0xA000, 0), backbuf, 64000u);
    } else if (scr.vesa && scr.pages >= 2) {
        /* Show the page just drawn without waiting: the new start address is picked up at the next
         * retrace, so the visible picture never tears.  The wait, if any, happens in plat_row. */
        union REGS r;
        struct SREGS s;
        segread(&s);
        r.w.ax = 0x4F07;
        r.w.bx = 0;
        r.w.cx = 0;
        r.w.dx = (unsigned)(scr.back * scr.yres);
        int10(&r, &s);
        t_flip[1] = t_flip[0];
        t_flip[0] = now_us();
        scr.back = (scr.back + 1) % (unsigned)scr.pages;
        need_wait = 1;
    }                                             /* single-page VESA: drawn directly */
    if (pal_pending) {
        set_palette(pending_pal);
        pal_pending = 0;
    }
}

const char *const *plat_help(void)
{
    static const char *const text[] = {
        "  -m MODE    use this VistaPro video mode, e.g. 0x13 or 0x101 (default: the file's,",
        "             or the smallest that fits); -modes shows which ones this display has",
        NULL
    };
    return text;
}

int plat_key(void)
{
    int c;
    if (!kbhit()) return KEY_NONE;
    c = getch();
    if (c == 0 || c == 0xE0) {
        c = getch();
        if (c == 75) return KEY_LEFT;
        if (c == 77) return KEY_RIGHT;
        if (c == 71) return KEY_HOME;
        return KEY_NONE;
    }
    if (c == 27 || c == 'q' || c == 'Q') return KEY_QUIT;
    if (c == ' ') return KEY_PAUSE;
    if (c == '+' || c == '=') return KEY_FASTER;
    if (c == '-') return KEY_SLOWER;
    return KEY_NONE;
}

u32 plat_ms(void)
{
    /* BIOS tick count, 18.2 Hz */
    return *(volatile unsigned long far *)MK_FP(0x40, 0x6C) * 55UL;
}

void plat_sleep(u32 ms)
{
    delay((unsigned)ms);
}

void plat_done(void)
{
    union REGS r;
    struct SREGS s;
    if (backbuf) {
        _ffree(backbuf);
        backbuf = NULL;
    }
    segread(&s);
    r.w.ax = 0x0003;
    int10(&r, &s);
}
