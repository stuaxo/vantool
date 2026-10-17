#!/bin/bash
# DOS build with Open Watcom: 16-bit real mode, large memory model, no DOS extender needed.
# Large locals are static and the stack is 8 KB (Watcom's 2 KB default overflowed, causing random hangs).
set -e
cd "$(dirname "$0")"
export WATCOM=${WATCOM:-/opt/watcom}
export PATH="$WATCOM/binl64:$WATCOM/binl:$PATH"
export INCLUDE="$WATCOM/h"
# -3: 386 instructions in 16-bit code (VistaPro itself needs a 386); much faster 32-bit arithmetic
# -D__WATCOM_LFN__: use the DOS long filename API (INT 21h AX=71xxh) when DOS provides it, as Windows 9x,
# DOSLFN, DOSBox-X and dosemu2 do; otherwise DOS shortens names as usual
OPTS="-q -bt=dos -ml -3 -ox -w4 -zq -k8192 -D__WATCOM_LFN__"
wcl $OPTS -fe=VANTOOL.EXE vantool.c van.c img.c flc.c sys_dos.c
wcl $OPTS -fe=VANPLAY.EXE vanplay.c play_dos.c flcread.c van.c sys_dos.c ring.c store_xms.c
rm -f *.o *.obj   # Watcom on a Linux host names objects .o
ls -l VANTOOL.EXE VANPLAY.EXE
