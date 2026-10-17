/* sys_posix.c - Linux process setup: nothing needed (SDL turns Ctrl-C into a quit event). */
#include "sys.h"

void sys_init(void (*on_break)(void))
{
    (void)on_break;
}

const char *sys_extract_note(void)
{
    return "";
}
