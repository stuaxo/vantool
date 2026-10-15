# vantool and vanplay

> **Guided slop.** An LLM was used to iterate this to output a spec, tool and player humans can use to
> understand the VistaPro animation format.

The tools build for DOS and Linux.


Tools:

- `vantool` inspects, extract, build, cuts and repair `.VAN` files.
- `vanplay` plays them.

Spec:

The spec is derived from Vistapro 3.20 for DOS, further work is needed to check other verrsions.

[VAN file specification](docs/VAN_SPEC.md).


## DOS requirements

- At least a 386
- DOS (DOS extenders are currently unsupported)
- VESA for resolutions beyond 320x200
- XMS (optional) for frame cache.


# Build requirements

gcc has been tested for building and SDL dev libraries are required for `vanplay`.

Open Watcom is required to build the DOS version.


## Build the programs

The programs are built in the `src` directory.

| Command | Builds |
|---|---|
| `src/build-linux.sh` | `vantool` and, if SDL2 is installed, `vanplay` |
| `src/build-dos.sh` | `VANTOOL.EXE` and `VANPLAY.EXE` (16-bit, large memory model, 386) |
| `make -C src` | `vantool` and `vanplay` with gcc and SDL2 |


## Look at a file

```
vantool info FILE.VAN
```

This shows the header fields, image size and number of frames. It also says whether the video mode
matches the image size and whether the palette uses 6-bit values.


## Extract frames as images

```
vantool extract FILE.VAN PREFIX [-t bmp|pcx] [-f FIRST] [-n COUNT]
```

This writes each frame as `PREFIXnnnn.BMP` or `PREFIXnnnn.PCX`. Frames are numbered from 0. Use `-f` to
choose the first frame and `-n` to choose how many frames to extract.

On DOS, use a prefix of 4 characters or fewer, because filenames are limited to 8 characters.


## Build a file from images

```
vantool make OUT.VAN [-m MODE] [-pc] [-k] IMAGE...
vantool make OUT.VAN [-m MODE] [-pc] [-k] -p PATTERN -n COUNT [-s START]
```

Images need to 8 bit BMP or PCX paletted images.

Numbered series of images are specified with `-p FR%04d.PCX`.
In DOS batch files, write `%` as `%%`.


`vantool make`:

- takes the palette from the first image and rounds it to the 6-bit levels the VGA uses
- stops if a later image has a different palette, unless you use `-k`
- writes the file as VistaPro 3.20 does, with rows stored bottom to top (`VANIMWN`)
- sets the video mode from the image size, or uses the mode you give with `-m`

If the image size does not match a VistaPro video mode, `vantool make` uses the smallest mode that fits.
VistaPro's VIEWER then shows the image in the middle of the screen.

Use `-pc` to store rows top to bottom (`VANIMPC`) instead.

For true-colour images, or images with different palettes, use `tools/vanfile.py make` instead. It builds
one palette shared by all frames.

Extracted palettes are stored as 8-bit values. `vantool make` converts them back to the original 6-bit
values exactly, so extracting a file and building it again gives an identical file.


## Cut part of a file

```
vantool slice IN.VAN OUT.VAN FIRST [COUNT]
```

This copies frames from `FIRST` onwards to a new file. Use `COUNT` to set how many frames to copy. If you
leave it out, all remaining frames are copied.

## Repair a file

```
vantool repair FILE.VAN [-pc]
```


This changes the file's header so that it describes its rows as bottom to top (`VANIMWN`). Use `-pc` to
describe them as top to bottom (`VANIMPC`). The frames are not changed.

You may need this because of a fault in VistaPro 3.20. When you ask it to continue an existing animation,
it only accepts `VANIMPC` files, but it writes `VANIMWN` files itself. To continue an animation:

1. Run `vantool repair FILE.VAN -pc`.
2. Continue the animation in VistaPro.
3. Run `vantool repair FILE.VAN` to set the header back to `VANIMWN`.

Step 3 matters because VistaPro keeps writing frames bottom to top, whatever the header says.


## Play a file

```
vanplay FILE.VAN [options]
```

| Option | What it does |
|---|---|
| `-r FPS` | frames per second (default 10) |
| `-l LOOPS` | number of times to play the file; 0 means repeat until you stop it (default 0) |
| `-p` | start paused |
| `-s FRAME` | start at this frame |
| `-n COUNT` | stop after showing this many frames |
| `-b KB` | memory to use for reading frames ahead, in KB (default 8192); 0 reads every frame from disk |
| `-v` | when the player stops, show how the display and memory were set up, and the frame rate achieved |
| `-t WHAT` | measure speed: 0 read and draw, 1 read only, 2 draw only, 3 through the read-ahead memory |
| `-m MODE` | DOS only: use this video mode, for example `0x13` or `0x101` |
| `-x SCALE` | Linux only: window size as a multiple of the image size |
| `-f` | Linux only: full screen |
| `-c OUT.BMP` | Linux only: save the last frame shown as a BMP image (used by the tests) |

| Key | What it does |
|---|---|
| Esc or Q | stop |
| Space | pause or resume |
| Left and Right arrows | go back or forward one frame |
| `+` and `-` | play faster or slower |
| Home | go to the first frame |

On DOS, `vanplay` shows the file in the video mode named in its header, as VistaPro's VIEWER does. If the
card does not support that mode, it uses the smallest VistaPro mode that fits. Images smaller than the
screen are shown in the middle.

On Linux, `vanplay` opens a window at the largest whole-number scale that fits 1280 by 1024 pixels. Colours
are converted from the file's 6-bit values the same way a VGA card does.


## Getting help on DOS

Run either program with `/?` to see its options.

If a disk error happens, for example a full disk or an empty drive, the program reports it and stops. It
does not show the DOS "Abort, Retry, Fail?" prompt. This has been built but not yet tested against a real
disk error.

If you press Ctrl-Break during playback, `vanplay` returns the screen to text mode and frees any extended
memory before it exits.


## How vanplay uses memory

`vanplay` plays `.VAN` files as they are. It does not compress or convert them. The only extra memory it
uses is a cache of frames, which has a fixed upper size.


### Extended memory (DOS) or main memory (Linux)

`vanplay` reads frames ahead into a cache of up to 8 MB. You can change the limit with `-b`. The cache uses
whichever is smallest:

- the limit
- the largest free block of extended memory
- the size of the file

If the whole file fits, it is read once and later loops play from memory. If it does not fit, the cache holds
the next few frames.

If there is no extended memory, or not enough for one frame, `vanplay` reads every frame from disk.


### Conventional memory (DOS)

`vanplay` uses one block of conventional memory for reading the file. It tries 60 KB first, then 32, 16, 8
and 4 KB, and always leaves 4 KB free. In mode 13h (320 by 200) it also uses 64 KB to build each frame off
screen. If there is not enough memory for this, it draws straight to the screen.


### Checking what was chosen

Use `-v` to see what `vanplay` chose and why. For example:

```
read-ahead 0 of 193 frames in none - no extended memory available; I/O block 32768 bytes
```

## How vanplay keeps playback smooth

Reading from disk is the slowest part of playback. Drawing is much faster.

`vanplay` does not wait idly between frames. While a frame is on screen, it reads the next frames into
the cache. It also does this while playback is paused.

If a frame is due before it has been fully read, `vanplay` shows the part already in memory, then reads the
rest from disk. It keeps a copy in the cache, so no frame is read from disk twice.


### Display on DOS

In VESA `vanplay` will use 1, 2 or 3 screen pages depending on how much memory the card has.
If there is only 1 page then screen tearing will occur.

For 320x200 animations `vanplay` will use mode 13h (classic VGA 320x200) with buffering when there is space.
