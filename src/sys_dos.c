/* sys_dos.c - DOS process setup: critical-error handler and Ctrl-C/Ctrl-Break handling. */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <dos.h>
#include "sys.h"

static void (*break_fn)(void);

/* INT 24h: fail the DOS call; the program sees an ordinary error return and reports it. */
static int __far hard_error(unsigned deverr, unsigned errcode, unsigned __far *devhdr)
{
    (void)deverr;
    (void)errcode;
    (void)devhdr;
    return _HARDERR_FAIL;
}

static void on_signal(int sig)
{
    (void)sig;
    if (break_fn)
        break_fn();
    fputs("\nInterrupted.\n", stderr);
    exit(3);
}

void sys_init(void (*on_break)(void))
{
    break_fn = on_break;
    _harderr(hard_error);
    signal(SIGINT, on_signal);
    signal(SIGBREAK, on_signal);
}
