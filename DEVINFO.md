## Dev info

Some info on display, performance and testing.


### VESA details

`vanplay`:

- uses 8-bit modes that the card reports as supported
- writes through window A, or window B on cards where only window B can be written to
- follows the card's bank size, line length and number of pages
- switches banks only when it needs to, by calling the card's bank-switch routine directly rather than
  through `INT 10h`
- copies to video memory 32 bits at a time
- copies cached frames from extended memory directly to video memory, without a copy in conventional memory
- sets the palette through the VGA ports, or through VESA function `4F09` if the card says its palette
  hardware is not VGA-compatible

To allow direct copies, the cache stores rows top to bottom. Files with rows stored bottom to top are
reordered as they are read ahead, not when the frame is shown.

The window B and `4F09` routes have not been tested, because the emulator used for testing does not need
them.

## Performance

All figures were measured in emulators. They show relative improvements. They do not predict speed on
real hardware.

### vanplay

Measured in dosbox-x set to about the speed of a 386DX-33 (`cycles=fixed 8000`).

| Test | Before VESA changes | After |
|---|---|---|
| Drawing only, 640 by 480 | 25.7 fps | 36.4 fps |
| Drawing only, 1280 by 1024 | 7.3 fps | 12.1 fps |
| From cache, 640 by 480 | 24 fps | 109 fps |
| From cache, 1280 by 1024, 3 pages | 7.8 fps | 54.5 fps |

The first time through a file, speed is limited by the disk, and did not change.

dosbox-x does not account for the time taken by extended memory copies or by the video card's bus. The
figures for playing from the cache are therefore higher than a real 386 would reach.

dosbox-x's default video card has only one page at 1280 by 1024. The 3-page figure used `vmemsize=4`.

### Low memory

Playback was checked at 15 fps, 640 by 480, with conventional and extended memory reduced. It kept the full
frame rate in every case.

- With 1 MB, 3 MB and no extended memory, the cache held 1, 8 and 0 of 12 frames.
- With no free conventional memory, the reading block dropped to 4 KB. The first time through, 8 frames
  were late, and the overall rate was 15.3 fps.

### vantool

Measured in dosemu2 with `pic.van`, which has 193 frames of 1280 by 1024 pixels (253 MB). Output was
identical before and after.

| Task | Before I/O changes | After |
|---|---|---|
| `slice` the whole file | 12.72 s | 0.35 s |
| `extract` 24 frames as BMP | 1.05 s | 0.10 s |
| `make` from 24 BMP images | 1.60 s | 0.10 s |
| `extract` 24 frames as PCX | 1.65 s | 0.60 s |
| `make` from 24 PCX images | 1.70 s | 0.90 s |

`vantool` reads and writes rows in the order they are stored where it can. Other access goes through a
block cache.

## Testing

Run:

```
python3 tools/test_vantool.py
python3 tools/test_vanplay.py
```

Build both the Linux and DOS programs first.

Use `--no-dos` to skip the DOS tests. `test_vanplay.py` also accepts `--no-sdl` to skip the Linux tests.

The tests need Python 3. The DOS tests also need:

- dosemu2, for `test_vantool.py`
- dosbox-x, Xvfb, xdotool and xwd, for `test_vanplay.py`

VistaPro itself is not included. Some tests use files from VistaPro 3.20. To run them, set `VISTAPRO` to
the directory that holds VistaPro:

```
VISTAPRO=/path/to/vistapro python3 tools/test_vantool.py
```

Without it, those tests are skipped:

- `test_vantool.py` skips the `VAN2FLC.EXE` check
- `test_vanplay.py` skips the 1280 by 1024 test, which uses frames from VistaPro's `pic.van`

`test_vantool.py` checks that:

- the Linux build gives the same results as the Python reference (`tools/vanfile.py`)
- the DOS build, run in dosemu2, gives byte-identical output to the Linux build
- VistaPro's own `VAN2FLC.EXE` reads files built by `vantool make` correctly
- every frame of each FLC and FLI file `vantool flc` writes, and the frame that loops back to the start,
  decodes exactly with `tools/flc.py` and, if it is installed, ffmpeg

The FLC and FLI files were also checked by eye in VistaPro's `PLAYFLC.EXE`, in dosbox-x. Screenshots
matched the frames pixel for pixel.

FLC files normally store changes between frames as SS2 chunks. SS2 keeps the last pixel of an odd-width
line in a special word, which `PLAYFLC.EXE` and ffmpeg both ignore. So for odd widths, `vantool` uses
LC chunks instead, which store bytes rather than pairs of bytes.

`test_vanplay.py` compares what each player shows, pixel by pixel, with the frames in the file. It captures
the Linux build without a window and takes screenshots of the DOS build in dosbox-x. It covers:

- mode 13h and VESA 640 by 400, 640 by 480 and 1280 by 1024
- files with rows stored either way
- images smaller than the screen, including an odd width
- the Right and Home keys, and stepping through frames from the cache

The Linux build is also tested on FLI and FLC files (not yet DOS: it needs Open Watcom, which is only on CI).
`tools/flcenc.py` is a test-only encoder that writes every chunk type the player reads, which `vantool flc` does not:
COPY and BLACK frames, partial palette changes, SS2 line skips and odd-width last pixels, LC inside an FLC, and a
chunk it must skip. The tests check that each frame and its palette are exact, that the ring frame loops back
to the first frame and play carries on after it, that files from `vantool flc` play the same as the `.VAN`
they came from, and that damaged files do not crash the player. The decoder was also run under
AddressSanitizer and UBSan on a few hundred damaged files.

### Testing Windows builds on Linux

Both test scripts accept `--exe` to test another build of the same program, and `--wrapper` to run it
through another program. To test the Windows builds with Wine:

```
python3 tools/test_vantool.py --no-dos --exe build/win64/vantool.exe --wrapper wine
python3 tools/test_vanplay.py --no-dos --exe build/win64/vanplay.exe --wrapper wine
```

Wine 10 as packaged by Ubuntu stops with `free(): invalid pointer` whenever `TMPDIR` is set, so the tests
remove `TMPDIR` from the environment of the program they run.

## Documentation

- [VAN file specification](docs/VAN_SPEC.md) describes the file format.
- [Reverse-engineering notes](docs/VAN_FORMAT.md) record the evidence behind the specification.

## Other tools

`tools/vanfile.py` is a Python reference implementation of the format. It needs no extra packages. It can
show file details, export frames as PNG images, build files from PNG or FLC files, cut and repair files.

`tools/gui` holds scripts for running DOS programs in dosbox-x and checking screenshots against `.VAN`
frames.

## Source files

All C source files are in `src`.

| File | Used on | Purpose |
|---|---|---|
| `van.c`, `van.h` | both | reading and writing `.VAN` files |
| `img.c`, `img.h` | both | reading and writing BMP and PCX images |
| `flc.c`, `flc.h` | both | writing FLC and FLI animations |
| `flcread.c`, `flcread.h` | both | reading FLC and FLI animations for `vanplay` |
| `vantool.c` | both | `vantool` |
| `vanplay.c` | both | `vanplay` playback loop, for `.VAN` and FLI/FLC files |
| `ring.c`, `ring.h` | both | frame cache for reading ahead |
| `store.h` | both | interface to cache memory |
| `store_xms.c` | DOS | cache in extended memory |
| `store_mem.c` | Linux, Windows | cache in main memory |
| `play.h` | both | interface to the display |
| `play_dos.c` | DOS | VGA and VESA display, timer and keyboard |
| `play_sdl.c` | Linux, Windows | SDL2 display |
| `sys.h`, `sys_dos.c` | DOS | disk error and Ctrl-Break handling |
| `sys_posix.c` | Linux, Windows | empty, not needed there |

The build scripts choose the files for each platform. The source has no `#ifdef` sections.

## Making changes

Work on a branch, not on `main`:

```
git switch -c feature/short-name
```

Push the branch and open a pull request into `main`. Merge it when the checks pass.

### Checks

GitHub Actions runs `.github/workflows/ci.yml` on every push and pull request. It:

- builds the Linux programs with gcc and SDL2, and runs both test scripts with `--no-dos`
- builds the DOS programs with Open Watcom 2.0
- builds the 32-bit and 64-bit Windows programs with mingw-w64, then runs both test scripts against them
  on a Windows runner

The DOS tests and the tests that need VistaPro do not run in CI. Run them locally before merging changes
to the DOS code.

Each run keeps the built programs for download, on the run's summary page.

### Releases

The version lives in `src/version.h`; both programs print it. To publish a release, set it there (as
`X.Y.Z`, and in `debian/changelog` too once that exists), merge that to `main`, then tag `main` and push
the tag:

```
git switch main
git pull
tools/check-version.sh v1.0.0
git tag v1.0.0
git push origin v1.0.0
```

CI runs `tools/check-version.sh` on every push, and on a tag it fails, and no release is made, unless
the tag matches `src/version.h`.

The checks run, then a GitHub release is created with:

| File | Contains |
|---|---|
| `vantdos.zip` | `VANTOOL.EXE`, `VANPLAY.EXE`, `README.TXT`, `VANSPEC.TXT` |
| `vantool-v1.0-linux-x86_64.tar.gz` | `vantool`, `vanplay`, `README.md`, `VAN_SPEC.md` |
| `vantool.zip` | FreeDOS package: `APPINFO\VANTOOL.LSM`, the programs and documents in `PROGS\VANTOOL`, and `LINKS` batch files that put both programs on the `PATH` |
| `vantool-v1.0-win32.zip`, `vantool-v1.0-win64.zip` | `vantool.exe`, `vanplay.exe`, `SDL2.dll`, `README.txt`, `VAN_SPEC.txt`, `SDL2-LICENSE.txt` |
| `vantool_1.0_amd64.deb` | `vantool` and `vanplay` in `/usr/bin`, documents in `/usr/share/doc/vantool` |

The two DOS zips have 8.3 names so they can be downloaded and used on DOS as they are. Their names do
not include the version; the release tag and the FreeDOS package's `APPINFO\VANTOOL.LSM` give it.

Text files in the DOS and Windows packages have DOS line endings, and those in the DOS package have 8.3
names. The Linux `vanplay` needs the SDL2
library (`libsdl2-2.0-0` on Debian and Ubuntu). The `.deb` lists its library dependencies, so install it
with `sudo apt install ./vantool_1.0_amd64.deb` to fetch them.

The `.deb` is built on GitHub's current Ubuntu runner, so it needs a system with that glibc version or
newer.

To build the same packages locally, build the programs, then run `tools/package.sh dos VERSION` or
`tools/package.sh freedos VERSION`, `tools/package.sh linux VERSION`, `tools/package.sh deb VERSION` or `tools/package.sh win64 VERSION`. The `.deb` needs `dpkg-dev`.
Packages are written to `dist`.
