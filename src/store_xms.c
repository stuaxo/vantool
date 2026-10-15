/* store_xms.c - vanplay frame store in XMS extended memory (DOS, via HIMEM.SYS or equivalent).
 * Data moves between conventional and extended memory with XMS function 0Bh, which the driver
 * performs in protected mode.  The block is freed in store_done(); vanplay also calls that from its
 * Ctrl-Break handler, since XMS left allocated stays lost until reboot.
 */
#include <string.h>
#include <i86.h>
#include "store.h"

static unsigned entry_seg, entry_off;     /* XMS driver entry point */
static unsigned handle;
static int have;

/* Call the XMS driver: AX = function, DX and DS:SI as needed.  Returns DX:AX. */
static unsigned long xms_call(unsigned ax, unsigned dx, unsigned si, unsigned ds_seg,
                              unsigned eseg, unsigned eoff);
#pragma aux xms_call = \
    "push ds"                \
    "push bp"                \
    "push es"                \
    "push bx"                \
    "mov bp, sp"             \
    "mov ds, cx"             \
    "call dword ptr ss:[bp]" \
    "add sp, 4"              \
    "pop bp"                 \
    "pop ds"                 \
    parm [ax] [dx] [si] [cx] [es] [bx] \
    value [dx ax]            \
    modify [bx cx es si];

static unsigned long xms(unsigned ax, unsigned dx)
{
    return xms_call(ax, dx, 0, 0, entry_seg, entry_off);
}

/* XMS move structure (function 0Bh).  Handle 0 means the offset is a real-mode seg:off. */
static struct {
    u32 length;
    unsigned src_handle;
    u32 src_offset;
    unsigned dst_handle;
    u32 dst_offset;
} mv;

static int move(void)
{
    void __far *p = (void __far *)&mv;
    return (unsigned)xms_call(0x0B00, 0, FP_OFF(p), FP_SEG(p), entry_seg, entry_off) == 1 ? 0 : 1;
}

static u32 real_ptr(const void __far *p)
{
    return ((u32)FP_SEG(p) << 16) | FP_OFF(p);
}

int store_init(u32 want, u32 *got)
{
    union REGS r;
    struct SREGS s;
    u32 kb;
    unsigned long res;

    *got = 0;
    if (!want)
        return 0;
    segread(&s);
    r.w.ax = 0x4300;
    int86x(0x2F, &r, &r, &s);
    if (r.h.al != 0x80)
        return 0;                             /* no XMS driver */
    r.w.ax = 0x4310;
    int86x(0x2F, &r, &r, &s);
    entry_seg = s.es;
    entry_off = r.w.bx;
    res = xms(0x0800, 0);                     /* AX = largest free block in KB */
    kb = (want + 2 + 1023) / 1024;            /* +2: room for the odd-length tail access */
    if (kb > (res & 0xFFFFu))
        kb = res & 0xFFFFu;
    if (kb > 65535UL)
        kb = 65535UL;
    if (!kb)
        return 0;
    res = xms(0x0900, (unsigned)kb);          /* allocate: AX = 1, DX = handle */
    if ((res & 0xFFFFu) != 1)
        return 0;
    handle = (unsigned)(res >> 16);
    have = 1;
    *got = kb * 1024UL - 2 < want ? kb * 1024UL - 2 : want;
    return 0;
}

static int put_even(u32 off, const unsigned char *src, unsigned n)
{
    mv.length = n;
    mv.src_handle = 0;
    mv.src_offset = real_ptr(src);
    mv.dst_handle = handle;
    mv.dst_offset = off;
    return move();
}

static int get_even(u32 off, unsigned char *dst, unsigned n)
{
    mv.length = n;
    mv.src_handle = handle;
    mv.src_offset = off;
    mv.dst_handle = 0;
    mv.dst_offset = real_ptr(dst);
    return move();
}

/* XMS moves must be an even length.  An odd tail byte goes through a 2-byte read-modify-write, so
 * nothing outside [off, off+n) (or outside dst) is ever touched. */
int store_put(u32 off, const unsigned char *src, unsigned n)
{
    unsigned char t[2];
    if (n > 1 && put_even(off, src, n & ~1u))
        return 1;
    if (!(n & 1))
        return 0;
    if (get_even(off + n - 1, t, 2))
        return 1;
    t[0] = src[n - 1];
    return put_even(off + n - 1, t, 2);
}

int store_get(u32 off, unsigned char *dst, unsigned n)
{
    unsigned char t[2];
    if (n > 1 && get_even(off, dst, n & ~1u))
        return 1;
    if (!(n & 1))
        return 0;
    if (get_even(off + n - 1, t, 2))
        return 1;
    dst[n - 1] = t[0];
    return 0;
}

void store_done(void)
{
    if (have) {
        xms(0x0A00, handle);                  /* free */
        have = 0;
    }
}

const char *store_kind(void)
{
    return "XMS";
}
