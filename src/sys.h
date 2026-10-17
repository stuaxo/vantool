/* sys.h - process-level setup that differs between DOS and Linux (sys_dos.c / sys_posix.c). */
#ifndef SYS_H
#define SYS_H

/* Call first thing in main().  on_break (may be NULL) runs before exiting on Ctrl-C/Ctrl-Break,
 * e.g. to restore text mode.  On DOS this also installs a critical-error handler, so disk full,
 * write-protect or drive-not-ready fail the call cleanly instead of showing Abort, Retry, Fail?. */
void sys_init(void (*on_break)(void));

/* Help text that only applies on this system, shown under vantool's "extract" usage; "" if none. */
const char *sys_extract_note(void);

#endif
