#!/bin/bash
# Linux build: gcc (needs libc6-dev; vanplay also needs libsdl2-dev).  If libc headers are missing,
# vantool falls back to Open Watcom -bt=linux and vanplay is skipped.  Force with TOOLCHAIN=gcc|watcom.
set -e
cd "$(dirname "$0")"
TOOLCHAIN=${TOOLCHAIN:-$( [ -f /usr/include/stdio.h ] && echo gcc || echo watcom )}
CFLAGS="-O2 -std=c89 -pedantic -Wall -Wextra -Wno-long-long"
if [ "$TOOLCHAIN" = gcc ]; then
    ${CC:-gcc} $CFLAGS -o vantool vantool.c van.c img.c flc.c sys_posix.c
    if pkg-config --exists sdl2; then
        ${CC:-gcc} $CFLAGS $(pkg-config --cflags sdl2) -o vanplay vanplay.c play_sdl.c van.c sys_posix.c ring.c store_mem.c $(pkg-config --libs sdl2)
    else
        echo "libsdl2-dev not found: skipping vanplay"
    fi
else
    export WATCOM=${WATCOM:-/opt/watcom}
    export PATH="$WATCOM/binl64:$WATCOM/binl:$PATH"
    INCLUDE="$WATCOM/lh" wcl386 -q -bt=linux -l=linux -ox -w4 -za99 -fe=vantool vantool.c van.c img.c flc.c sys_posix.c
    rm -f *.o *.obj
    echo "no libc headers: vanplay (SDL2) skipped"
fi
echo "built with $TOOLCHAIN"; ls -l vantool vanplay 2>/dev/null || true
