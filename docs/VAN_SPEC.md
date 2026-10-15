# VistaPro VANIM (.VAN) File Format Specification

| | |
|---|---|
| Spec revision | 0.6 (draft) |
| Last updated | 2026-10-14 |
| Covers | VistaPro 3.20 for DOS (`VP3.EXE`), VAN2FLC 3.10, VIEWER 3.10 |
| Evidence | `VAN_FORMAT.md` (reverse-engineering notes, code addresses) |

Each fact carries a status tag:

- **[C]** Confirmed: seen in the disassembly and in a real sample, or checked by running the original tool.
- **[S]** Static: seen in the disassembly only.
- **[I]** Inferred: consistent with the evidence but not proven.
- **[?]** Unknown.

The key words MUST, SHOULD and MAY mean what they mean in RFC 2119.

---

## 1. Overview

A VANIM file holds a fixed-size, 256-colour animation that VistaPro renders one frame at a time. It
consists of a fixed header, one palette for the whole animation, and uncompressed 8-bit indexed frames
stored back to back. There is no compression, no per-frame header, no frame index and no stored frame
count. **[C]**

VistaPro appends each frame to the file as soon as it finishes rendering it, so a file is valid after any
number of complete frames, including zero. **[C]** While a frame is being written, the file can briefly
end in a partial frame (observed mid-render). Aborting a render with Esc happens between frames, so it
leaves only whole frames. **[C]**

## 2. Conventions

- All multi-byte integers are unsigned 32-bit little-endian (`u32`). **[C]**
- Offsets are counted from the start of the file.
- Pixel values are indexes into the palette.

## 3. File layout

| Offset | Size | Name |
|---|---|---|
| `0x000` | 256 | Header (§4) |
| `0x100` | 768 | Palette (§5) |
| `data_offset` | `frame_size` × *N* | Frame data (§6) |

In every file written by VP3 3.20, `data_offset` is `0x400`, so frame data follows the palette directly.
**[C]**

## 4. Header

| Off | Type | Name | VP3 3.20 writes | Status | Description |
|---|---|---|---|---|---|
| `0x00` | `char[8]` | `magic` | `"VANIMWN\0"` | [C] | Identifies the file and the row order (§4.1) |
| `0x08` | u32 | `image_bytes` | `width*height` | [C] | Pixel count of one image. Readers SHOULD NOT depend on it |
| `0x0C` | u32 | `data_offset` | `0x400` | [C] | File offset of frame 0. VIEWER ignores it and hardcodes `0x400` (§7) |
| `0x10` | u32 | `frame_size` | `width*height` | [C] | Bytes per stored frame; the frame stride |
| `0x14` | u32 | `colors` | `256` | [C] meaning [I] | Probably the palette entry count. Always 256 |
| `0x18` | u32 | `width` | image width | [C] | Pixels per row |
| `0x1C` | u32 | `height` | image height | [C] | Number of rows |
| `0x20` | u32 | `unk20` | `1` | [C] meaning [?] | Always 1. Possibly bytes per pixel or a version |
| `0x24` | u32 | `video_mode` | rendering video mode | [C] | BIOS or VESA mode number, e.g. `0x13` = 320×200 VGA, `0x107` = 1280×1024 VESA. VIEWER switches to this mode for playback and refuses to play if the mode is unavailable |
| `0x28` | u32 | — | `0` | [C] | Reserved |
| `0x2C` | 212 bytes | — | all zero | [C] | Reserved |

### 4.1 Magic and variants

| Magic | Row order | Written by | Read by |
|---|---|---|---|
| `"VANIMWN\0"` | bottom-up (first stored row is the bottom of the image) | VP3 3.20 **[C]** | VAN2FLC 3.10 **[C]** |
| `"VANIMPC\0"` | top-down **[C]** | not observed; presumably an earlier VistaPro **[?]** | VAN2FLC 3.10 **[C]**; VIEWER 3.10 **[C]**; VP3 3.20 "continue" **[C]** |

All three programs compare the magic with `strcmp`, so the terminating NUL is part of it. **[S]** A synthetic
`VANIMPC` file with top-down rows converts through VAN2FLC to the same pixels as the `VANIMWN` original. **[C]**

## 5. Palette

- 256 entries × 3 bytes, in the order R, G, B. **[C]**
- Values are 6-bit VGA DAC levels, `0..63`. VP3 copies the hardware DAC directly. **[S]**
- To convert to 8-bit, VAN2FLC uses `v << 2`, which gives `0..252`. **[C]** Tools that need output identical
  to VAN2FLC MUST do the same. Other tools MAY use `(v << 2) | (v >> 4)` for full-range output.

## 6. Frame data

- Frame *i* starts at `data_offset + i * frame_size`. **[C]**
- Each frame is `height` rows of `width` bytes, one palette index per pixel. **[C]**
- Row order depends on the magic (§4.1). For `VANIMWN`, the row stored at position *k* is image row
  `height-1-k`. **[C]**
- Frame count = `floor((file_size - data_offset) / frame_size)`. **[C]** VP3 computes it the same way when
  it resumes a render. **[S]**

### 6.1 Truncated files

A trailing partial frame (fewer than `frame_size` bytes) can occur if a render was interrupted mid-write.
VAN2FLC stops at the first short read, so it ignores a partial frame. **[S]** Readers SHOULD ignore it too
and MAY warn about it.

## 7. Reader requirements

A conforming reader:

1. MUST check `magic` against both values in §4.1.
2. MUST use `data_offset` and `frame_size` from the header and MUST NOT assume `0x400` or `width*height`.
   (VIEWER 3.10 does assume `0x400`; writers MUST NOT rely on readers honouring other values.)
3. MUST use `width` and `height` for image geometry. If `frame_size > width*height`, the bytes beyond
   `width*height` in each frame SHOULD be ignored. This case has not been observed **[?]**.
4. MUST apply the row order for the variant.
5. SHOULD reject files where `width*height > frame_size`, or where `width` or `height` is 0.

## 8. Writer requirements

To produce files that VistaPro tools accept:

1. MUST zero the full 256-byte header, then fill in the fields in §4.
2. SHOULD write `VANIMWN` with bottom-up rows, which is what VP3 3.20 and VAN2FLC handle. `VANIMPC` is
   accepted by VAN2FLC, but its row order has not been verified at runtime.
3. MUST set `data_offset = 0x400`, `frame_size = image_bytes = width*height`, `colors = 256` and
   `unk20 = 1`.
4. SHOULD set `video_mode` to a real BIOS/VESA mode that matches `width`×`height` (§9.4). VIEWER switches to
   this mode and refuses to play otherwise. VAN2FLC ignores it. A smaller image MAY use the smallest mode that
   fits it; VIEWER centres it. **[C]**
5. SHOULD write at least 2 frames if the file is meant to play in VIEWER. VIEWER returns to DOS straight
   away for a 1-frame file. VAN2FLC handles 1 frame fine. **[C]**
6. MUST store palette values as 6-bit (`0..63`).
7. Appending whole frames to an existing valid file keeps it valid.

Reference implementation: `tools/vanfile.py` (`VanWriter`). Re-writing VP3 output through it gives a
byte-identical file. **[C]** This was checked on `pic.van` (1280×1024, mode `0x107`) and on a 640×480
file (mode `0x101`, 171 frames) that VP3 wrote in dosbox-x.

## 9. Producer and consumer behaviour

### 9.1 VP3.EXE 3.20 (producer)

- Started from Script > Run VAN (asks for a script, then a base name with `.VAN` appended, then "Run a batch
  file after each frame?"). **[C]**
- When saving a frame: opens the file for append. If that fails, creates it and writes the header and palette.
  Then appends the frame, writing rows from `height-1` down to `0`. **[C]**
- If the `.VAN` already exists, VP3 asks "Continue existing animation or delete it?". Continue reads the
  header, accepts it only if `magic` is `"VANIMPC"`, and requires `width` and `height` to match the current
  video mode. The number of frames already done is `(file_size - data_offset) / frame_size`, and the script
  resumes from that frame. **[C]**
- **Bug:** VP3 3.20 rejects its own `VANIMWN` files on continue ("Not a valid VANIM file!"). **[C]**
- If the magic is patched to `VANIMPC`, continue is accepted and the script resumes correctly (frame 171
  carries on from 170). The new frames are still written **bottom-up** and the header is left unchanged,
  so the file ends up claiming top-down rows over bottom-up data. **[C]** Tools SHOULD offer a way to
  repair such files, e.g. rewriting the magic to `VANIMWN`.
- Inference: an earlier VistaPro DOS wrote `VANIMPC` (VIEWER and VAN2FLC 3.10 read it as top-down). The 3.20
  writer switched to `VANIMWN`, but the continue check was never updated. **[I]**

### 9.2 VAN2FLC.EXE 3.10 (consumer)

- Reads the header and palette, seeks to `data_offset`, then reads `frame_size` bytes per frame until a
  short read. Flips rows if the magic is `VANIMWN`. **[C]**
- Output: an FLC (`0xAF12`) with the same width and height, 8 bpp, and the palette scaled with `<< 2`.
  Frame 1 is a BRUN chunk, later frames are SS2 deltas (or all BRUN with `+b`), and it ends with a
  loop-back frame back to frame 0. **[C]**
- **Bug:** for odd widths, the SS2 delta encoder never updates the last pixel column. It marks the line
  but emits no packet and no `0x8000` last-pixel word, so that column keeps stale pixels. `+b` (BRUN only) is
  correct. Every VP3 mode has an even width, so VistaPro's own files are not affected. **[C]** (33×7 and 35×7
  test files)

### 9.3 VIEWER.EXE 3.10 (consumer, DOS player)

`Viewer FILE.VAN [FILE.MID] [-s#] [-l#]` **[C]**

Behaviour below marked [C] was checked by playing generated files in dosbox-x and comparing screen captures
pixel for pixel (`tools/gui/vancheck.py`): 320×200 (`0x13`), 640×480 `WN` and `PC` (`0x101`), 400×300 in
`0x100`, and a median-cut quantised gradient. All were 100% exact.

- Reads the 256-byte header and checks the magic (`VANIMPC` gives top-down, `VANIMWN` gives bottom-up),
  then reads the 768-byte palette. **[S]**
- Ignores `data_offset`: frame count = `(file_size - 0x400) / frame_size`, and frame *i* is read from
  `0x400 + i*frame_size`. **[S]**
- If it can allocate `file_size - 0x400` bytes, it loads every frame into memory ("Loading %d frames...").
  Otherwise it streams frames from disk using a `width*height` buffer. **[S]**
- Sets video mode `video_mode` and fails with "Video mode %x not available" if it can't. **[C]** for setting
  the mode; the failure path is [S].
- Centres the image on screen when it is smaller than the mode's resolution: 400×300 in 640×400 is drawn at
  (120, 50). **[C]**
- Displays the palette through the VGA DAC, so a 6-bit value `v` appears as 8-bit `(v<<2)|(v>>4)`. **[C]**
- With only 1 frame, it exits to DOS immediately. **[C]**

### 9.4 Video modes known to VP3 3.20

VP3 has exactly six display modes. The animation is recorded at the current mode's resolution, and its
mode number goes into `video_mode`. **[S]** (mode-setup switch, VP3 file 0x307C1)

| VP3 index | `video_mode` | width × height | Type |
|---|---|---|---|
| 0 | `0x13` | 320 × 200 | VGA BIOS mode |
| 1 | `0x100` | 640 × 400 | VESA |
| 2 | `0x101` | 640 × 480 | VESA |
| 3 | `0x103` | 800 × 600 | VESA |
| 4 | `0x105` | 1024 × 768 | VESA |
| 5 | `0x107` | 1280 × 1024 | VESA ([C] `pic.van`) |

Writers creating files for VIEWER SHOULD use one of these pairs.

## 10. Testing and conformance

The prototype tools give three independent checks:

| Check | Tool | What it shows |
|---|---|---|
| Round trip | `vanfile.py make OUT.VAN X.FLC` on an FLC from VAN2FLC | VAN → FLC → VAN is byte-identical **[C]** |
| VAN2FLC end-to-end | `tools/e2e_van2flc.py FILE.VAN... [--flags +b\|+s]` | VAN2FLC, run headless in dosemu2, decodes every frame, the loop-back frame and the palette exactly as our reader does |
| VIEWER on screen | `tools/gui/run-dos.sh DIR "VIEWER F.VAN -s1 -l99"` + `tools/gui/vancheck.py` | Playback in the original player is pixel-exact |
| Players | `tools/test_vanplay.py` | `vanplay` (SDL, captured headless) and `VANPLAY.EXE` (dosbox-x screenshots) display every tested mode and variant pixel-exact |
| C tools | `tools/test_vantool.py` | `vantool` (Linux and DOS builds) against the Python reference, DOS output byte-identical to Linux, and VAN2FLC end-to-end on its output (99 checks) |

`vanfile.py testpattern` generates files for these checks. Each frame has a 1-pixel border, a red top-left
marker, a blue bottom-right marker, the frame number, a 16×16 grid of palette indexes, and a moving bar.
Orientation, palette, cropping and frame-order mistakes show up at a glance.

The end-to-end test is known to catch errors: a `PC` header over bottom-up rows fails on about 30% of
pixels per frame.

## 11. Example

`pic.van` (VistaPro 3.20 sample):

```
00: 56 41 4E 49 4D 57 4E 00   magic        "VANIMWN\0"
08: 00 00 14 00               image_bytes  0x140000 (1310720)
0C: 00 04 00 00               data_offset  0x400
10: 00 00 14 00               frame_size   0x140000
14: 00 01 00 00               colors       256
18: 00 05 00 00               width        1280
1C: 00 04 00 00               height       1024
20: 01 00 00 00               unk20        1
24: 07 01 00 00               video_mode   0x107 (VESA 1280x1024x256)
28..FF: zero
```

File size 252,969,984 = 0x400 + 193 × 1,310,720, so 193 frames with no partial frame.

## 12. Open issues

| # | Issue | How to resolve |
|---|---|---|
| 1 | Meaning of `colors` (0x14) and `unk20` (0x20) | No reader of the three uses them; check other VistaPro versions |
| 2 | `frame_size != width*height` | Check readers for padding handling |
| 3 | Other VistaPro versions (Windows VistaPro 3/4, Amiga?) | Get binaries or samples |

## Changelog

- **0.6** (2026-10-14): §10 lists the player tests.
- **0.5** (2026-10-14): VAN2FLC odd-width SS2 bug (§9.2); C tool tests added to §10.
- **0.4** (2026-10-14): VAN files generated by our writer verified in VIEWER (pixel-exact screen captures) and
  VAN2FLC (headless end-to-end, all modes); VIEWER centring, DAC scaling and 1-frame behaviour; new §10
  Testing.
- **0.3** (2026-10-14): runtime-tested VP3 3.20 in dosbox-x: writer fields confirmed (640×480, `0x101`);
  continue bug confirmed; behaviour of continue on a `VANIMPC` file recorded; partial-frame behaviour.
- **0.2** (2026-10-14): full VP3 video mode table (§9.4). VIEWER 3.10 analysed (hardcoded `0x400`, uses `video_mode`); `VANIMPC` top-down
  confirmed at runtime; resolved open issue "row order of `VANIMPC`".
- **0.1** (2026-10-14): first draft, from VAN2FLC 3.10 and VP3 3.20, checked against `pic.van` and
  against VAN2FLC run under dosemu.
