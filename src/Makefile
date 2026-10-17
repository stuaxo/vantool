# Linux build (gcc + SDL2 for vanplay).  DOS build: ./build-dos.sh (Open Watcom, 16-bit real mode).
CC ?= gcc
CFLAGS ?= -O2
override CFLAGS += -std=c89 -pedantic -Wall -Wextra -Wno-long-long
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS := $(shell pkg-config --libs sdl2)

all: vantool vanplay

vantool: vantool.o van.o img.o flc.o sys_posix.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

vanplay: vanplay.o play_sdl.o flcread.o van.o sys_posix.o ring.o store_mem.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(SDL_LIBS)

play_sdl.o: play_sdl.c play.h van.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SDL_CFLAGS) -c $<

%.o: %.c van.h img.h flc.h flcread.h play.h sys.h ring.h store.h version.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f vantool vanplay *.o *.obj *.err VANTOOL.EXE VANPLAY.EXE

.PHONY: all clean
