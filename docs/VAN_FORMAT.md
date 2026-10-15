# VistaPro VANIM (.VAN) format

> Reverse-engineering evidence log. The clean, implementable spec is `docs/VAN_SPEC.md`; update both together.

Recovered from `VAN2FLC.EXE` v3.10 (John Hinkley, 1993-94), which reads VAN, and from `VP3.EXE` 3.20, which
writes it. Checked against `vistapro/pic.van`. The prototype reader is `tools/vanfile.py`.

**Verified:** `VAN2FLC T4.VAN T4.FLC` was run under dosemu2 on the first 4 frames of `pic.van`. Decoding
its FLC output with `tools/flc.py` matched `vanfile.py` byte for byte: every frame, the loop-back
frame and the palette.

## Layout

| Offset | Size | Contents |
|---|---|---|
| 0x000 | 256 | Header |
| 0x100 | 768 | Palette: 256 x (R,G,B), 6-bit VGA values (0..63) |
| `data_offset` | `frame_size` x N | Frames, stored raw, back to back until EOF |

There is no per-frame header and no compression. The frame count comes from the file size and is
not read from the header.

## Header (little-endian)

| Off | Type | `pic.van` | Meaning | Source |
|---|---|---|---|---|
| 0x00 | char[8] | `VANIMWN\0` | Magic: `VANIMPC\0` or `VANIMWN\0` | VAN2FLC strcmp at 0x5808; VP3 writes WN |
| 0x08 | u32 | 0x140000 | Image bytes: width*height | VP3 writer |
| 0x0C | u32 | 0x400 | data_offset: start of frame data | VP3 writes the constant 0x400 |
| 0x10 | u32 | 0x140000 | frame_size: width*height | VP3 writer; VAN2FLC read size |
| 0x14 | u32 | 0x100 | Constant 256, probably the palette entry count | VP3 writer |
| 0x18 | u32 | 1280 | width | both |
| 0x1C | u32 | 1024 | height | both |
| 0x20 | u32 | 1 | Constant 1, perhaps bytes per pixel | VP3 writer |
| 0x24 | u32 | 0x107 | Video mode the frames were rendered in (0x13 = VGA 320x200; 0x107 = VESA 1280x1024x256) | VP3 global set in the mode-setup switch |
| 0x28 | u32 | 0 | Written explicitly as 0 | VP3 writer |
| 0x2C-0xFF | | 0 | Zero-filled | VP3 zeroes the whole 256 bytes |

## Variants

- `VANIMWN`: rows are stored bottom-up, like a Windows DIB. VAN2FLC swaps row *y* with row *h-1-y*
  (routine at file 0x5A09) whenever this magic is present.
- `VANIMPC`: rows are stored top-down, so no flip is needed.

## Palette

VAN2FLC converts the palette for FLC output with `b << 2`, with no low-bit fill.

## VAN2FLC read loop (file offsets in VAN2FLC.EXE)

1. `0x57B1` open, read 256-byte header, check magic, read 768-byte palette, scale it.
2. `0x592D` copy width/height/frame_size into globals; allocate 2 frame buffers and a
   2*frame_size encode buffer.
3. `0x6358` lseek(data_offset); read frame 0 (flip if WN); write the first FLC frame (BRUN).
4. Loop: swap buffers, read the next frame_size, flip, delta-encode (SS2) against the previous
   frame, write it. A short read ends the loop.
5. Seek back to data_offset, re-read frame 0 and emit a loop-back delta frame (standard FLC ring
   frame).

## Binary notes

- VAN2FLC is built with Watcom C/386 and linked with the FlashTek X-32VM extender (Doug Huffman).
  The MZ stub is 0x600 bytes, and the 32-bit image follows it.
- In the flat image, data address = file offset - 0x5179 for VAN2FLC and - 0x51C9 for VP3.EXE (same
  extender). Code offsets above are raw file offsets (from `ndisasm -b32 -e 0x680 -o 0x680`).

## VP3.EXE writer (file offset 0x3862D)

1. `open(name, O_WRONLY|O_APPEND|O_BINARY)`. If that fails, `open(name, O_WRONLY|O_CREAT|O_TRUNC|O_BINARY, 0600)`,
   then write the header and palette. Recording frames therefore appends to an existing file.
2. Header: zero 256 bytes, then fill in the fields above. The magic is always `VANIMWN`.
3. Palette: 768 bytes from the current VGA DAC (6-bit).
4. Frame: write rows `h-1` down to `0` from the frame buffer, `width` bytes each, so rows end up bottom-up.

The "Continue existing animation or delete it?" path (0x33BD9) reads the header and `strcmp`s it against
`VANIMPC` only. If that fails it reports "Not a valid VANIM file!". It also checks width/height against the
current mode ("Wrong image size for VANIM!"), then computes the frames already done as
`(lseek(END) - hdr[0x0C]) / hdr[0x10]`. As read statically, VP3 3.20 rejects its own `VANIMWN` files on
continue. This has not been tested at runtime.

## VIEWER.EXE 3.10 (file offsets; data address = file offset - 0xA8F9)

- `0xCA8B` read 256-byte header into 0x793C; `strcmp` against `VANIMPC` (sets flag 0x61A8=1) or `VANIMWN`
  (flag=0); otherwise prints `"%s" is not a valid VANIM file!`.
- `0xCB2F` read 768-byte palette; `lseek(END) - 0x400` (hardcoded, not hdr+0x0C) / hdr+0x10 = frame count.
- `0xCB86` malloc(whole body) and load all frames ("Loading %d frames..."), else malloc(w*h) and stream
  (`0xC6FD`: seek `0x400 + i*frame_size`).
- `0xCC6E` set mode hdr+0x24 (`0xC519`); on failure "Video mode %x not available".
- `0xC601` blit: centred; flag=1 copies source rows forward, flag=0 starts at the last row and goes backwards.

## VP3 video modes (switch at file 0x307C1, jump table at linear 0x2B5DC)

VP3 code and data share one base: linear = file offset - 0x51C9. Cases set 0x5CA9C (mode), 0x5CAAC (w),
0x5CAB0 (h): 0 = 0x13 320x200, 1 = 0x100 640x400, 2 = 0x101 640x480, 3 = 0x103 800x600,
4 = 0x105 1024x768, 5 = 0x107 1280x1024. The VESA cases first call 0x305D1(mode) and bail out on -2.

## Runtime checks

- `VANIMPC` synthetic 2-frame file (top-down rows, written by `VanWriter(..., 'PC')`), converted with
  VAN2FLC under dosemu, decodes to the same frames as the `WN` original. So PC = top-down, confirmed.

- VP3 3.20 under dosbox-x (`tools/gui/`), Script > Open ALPS.SCR, Script > Run VAN, base name PIC, no
  batch file:
  - It rendered a 640x480 `VANIMWN` file with header `08=0x4B000 0C=0x400 10=0x4B000 14=0x100 18=640
    1C=480 20=1 24=0x101`. Esc aborted it after 171 whole frames. A file listing taken mid-render showed a
    size that was not a whole number of frames (a partial write in progress). `VanWriter` re-encodes it
    byte-identical.
  - Running it again on the same name and choosing Continue gave "Not a valid VANIM file!". So the bug is real.
  - Patching the magic to `VANIMPC` and choosing Continue was accepted. VP3 resumed at frame 171 and 25 frames
    were appended. Similarity check: stored frame 171 matches stored 170 about 75% as-is and about 10% flipped,
    so the appended frames are bottom-up like the rest. The header magic was left as `VANIMPC`.

- Writer tests (2026-10-14): `vanfile.py testpattern`/`make` output played in VIEWER under dosbox-x and
  compared to the expected frame with `tools/gui/vancheck.py`: TP320 (0x13), TP640 WN and PC (0x101), TP400
  centred at (120,50) in 0x100, GRAD2 (median-cut): all 100% pixel-exact. A 1-frame GRAD.VAN makes VIEWER exit
  immediately (consistent with its `frames-1` loop at 0xCB70). `e2e_van2flc.py` passes for all of these with
  default, `+b` and `+s` flags; a PC-header/bottom-up file fails as expected.

- VAN2FLC odd-width bug: on a 33x7 test file, frame 1's SS2 chunk is `lines=2: FFFD (skip 3) 0000 0000`.
  Rows 3 and 4 are listed with 0 packets and no 0x8000 last-byte word, but their only change is in column 32.
  The 34x7 file passes, and 33/35 pass with `+b`.

## Open questions

- Which VistaPro version wrote `VANIMPC`. No sample is available.
- The meaning of the constant fields 0x14 and 0x20.
