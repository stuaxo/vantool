#!/bin/bash
# Windows build, cross-compiled with mingw-w64.  Output goes to ../build/win32 or ../build/win64.
#   SDL2_DIR=/path/to/SDL2-2.x.y src/build-windows.sh 32|64
# SDL2_DIR is the unpacked SDL2-devel-2.x.y-mingw.tar.gz; without it vanplay is skipped.
set -e
cd "$(dirname "$0")"
case "${1:?usage: build-windows.sh 32|64}" in
32) TRIPLE=i686-w64-mingw32 ;;
64) TRIPLE=x86_64-w64-mingw32 ;;
*) echo "usage: build-windows.sh 32|64" >&2; exit 1 ;;
esac
OUT=../build/win$1
mkdir -p "$OUT"
CC=${CC:-$TRIPLE-gcc}
# -static-libgcc: no libgcc DLL to ship.  sys_posix.c is plain C and does nothing, which suits Windows too.
CFLAGS="-O2 -std=c89 -pedantic -Wall -Wextra -Wno-long-long -static-libgcc"
$CC $CFLAGS -o "$OUT/vantool.exe" vantool.c van.c img.c flc.c sys_posix.c
if [ -n "$SDL2_DIR" ]; then
    SDL="$SDL2_DIR/$TRIPLE"
    $CC $CFLAGS -mconsole -I"$SDL/include/SDL2" -o "$OUT/vanplay.exe" vanplay.c play_sdl.c flcread.c van.c sys_posix.c ring.c store_mem.c \
        -L"$SDL/lib" -lmingw32 -lSDL2main -lSDL2
    cp "$SDL/bin/SDL2.dll" "$OUT"
else
    echo "SDL2_DIR not set: skipping vanplay"
fi
$TRIPLE-strip "$OUT"/*.exe
ls -l "$OUT"
