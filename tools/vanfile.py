#!/usr/bin/env python3
"""Prototype tool for VistaPro VANIM (.VAN) animation files.

Format (see docs/VAN_SPEC.md):

  0x000  256-byte header (little-endian dwords after an 8-byte magic)
  0x100  768-byte palette, 256 x RGB, 6-bit VGA values (0..63)
  data_offset (header +0x0c, normally 0x400)
         frames, each exactly frame_size bytes of raw 8-bit pixels,
         read back to back until EOF.  "VANIMWN" files store rows
         bottom-up; "VANIMPC" files store them top-down.

Stdlib only.  Usage:
  vanfile.py info   FILE.VAN
  vanfile.py png    FILE.VAN OUTDIR [--first N] [--count N] [--step N]
  vanfile.py slice  FILE.VAN OUT.VAN [--first N] [--count N]
  vanfile.py make   OUT.VAN INPUT... [--mode M] [--variant WN|PC]   (INPUT: .png frames or .flc)
  vanfile.py testpattern OUT.VAN [--size WxH] [--frames N] [--mode M]
  vanfile.py repair FILE.VAN [--variant WN|PC]   rewrite the magic in place (no pixel change)
"""
import argparse
import os
import struct
import sys

from imgio import quantize, read_png, write_png

MAGICS = {b"VANIMPC\0": "PC", b"VANIMWN\0": "WN"}
HEADER_SIZE = 0x100
PALETTE_SIZE = 0x300

# VP3 3.20 display modes (spec section 9.4): video_mode -> (width, height)
VP3_MODES = {0x13: (320, 200), 0x100: (640, 400), 0x101: (640, 480),
             0x103: (800, 600), 0x105: (1024, 768), 0x107: (1280, 1024)}


def pick_mode(width, height):
    """Exact VP3 mode for the size, else the smallest that fits (VIEWER centres the image)."""
    for m, (w, h) in VP3_MODES.items():
        if (w, h) == (width, height):
            return m, True
    fits = [(w * h, m) for m, (w, h) in VP3_MODES.items() if w >= width and h >= height]
    if not fits:
        raise ValueError(f"{width}x{height} is larger than any VP3 mode")
    return min(fits)[1], False


class VanFile:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        hdr = self.f.read(HEADER_SIZE)
        if len(hdr) != HEADER_SIZE:
            raise ValueError("file too short for VANIM header")
        magic = hdr[:8]
        if magic not in MAGICS:
            raise ValueError(f"bad magic {magic!r}")
        self.variant = MAGICS[magic]
        # dwords at 0x08..0x24, as written by VP3.EXE 3.20
        (self.image_bytes, self.data_offset, self.frame_size, self.colors,
         self.width, self.height, self.unk20, self.video_mode) = struct.unpack_from("<8I", hdr, 8)
        self.header = hdr
        pal6 = self.f.read(PALETTE_SIZE)
        if len(pal6) != PALETTE_SIZE:
            raise ValueError("file too short for palette")
        self.palette6 = pal6
        # VAN2FLC does `b << 2` (no low-bit fill); do the same for fidelity
        self.palette8 = bytes((b << 2) & 0xFF for b in pal6)
        size = os.fstat(self.f.fileno()).st_size
        body = size - self.data_offset
        self.frame_count = body // self.frame_size if self.frame_size else 0
        self.trailing_bytes = body - self.frame_count * self.frame_size

    def read_frame(self, index):
        """Return raw pixel bytes for a frame, top-down rows."""
        if not 0 <= index < self.frame_count:
            raise IndexError(index)
        self.f.seek(self.data_offset + index * self.frame_size)
        data = self.f.read(self.frame_size)
        pixels = data[: self.width * self.height]
        if self.variant == "WN":
            w = self.width
            rows = [pixels[y * w:(y + 1) * w] for y in range(self.height)]
            pixels = b"".join(reversed(rows))
        return pixels


class VanWriter:
    """Write a VANIM file the way VP3.EXE 3.20 does (VANIMWN by default)."""

    def __init__(self, path, width, height, palette6, video_mode=0, variant="WN"):
        if len(palette6) != PALETTE_SIZE or max(palette6) > 63:
            raise ValueError("palette must be 768 bytes of 6-bit values")
        self.width, self.height, self.variant = width, height, variant
        hdr = bytearray(HEADER_SIZE)
        hdr[:8] = b"VANIMWN\0" if variant == "WN" else b"VANIMPC\0"
        n = width * height
        struct.pack_into("<9I", hdr, 8, n, 0x400, n, 0x100, width, height, 1, video_mode, 0)
        self.f = open(path, "wb")
        self.f.write(hdr)
        self.f.write(bytes(palette6))

    def write_frame(self, pixels):
        """pixels: top-down rows, width*height bytes."""
        w, h = self.width, self.height
        if len(pixels) != w * h:
            raise ValueError("frame size mismatch")
        if self.variant == "WN":
            for y in range(h - 1, -1, -1):
                self.f.write(pixels[y * w:(y + 1) * w])
        else:
            self.f.write(pixels)

    def close(self):
        self.f.close()


def cmd_info(args):
    v = VanFile(args.file)
    print(f"file          {v.path}")
    print(f"variant       VANIM{v.variant} ({'bottom-up rows' if v.variant == 'WN' else 'top-down rows'})")
    print(f"size          {v.width} x {v.height}")
    print(f"frame_size    {v.frame_size:#x} ({v.frame_size})")
    print(f"data_offset   {v.data_offset:#x}")
    print(f"frames        {v.frame_count}" + (f"  (+{v.trailing_bytes} trailing bytes)" if v.trailing_bytes else ""))
    print(f"image_bytes   {v.image_bytes:#x}")
    print(f"colors        {v.colors}")
    print(f"unk20         {v.unk20:#x}  (VP3 always writes 1)")
    mode = VP3_MODES.get(v.video_mode)
    print(f"video_mode    {v.video_mode:#x}" + (f"  ({mode[0]}x{mode[1]})" if mode else "  (not a VP3 mode)"))
    nonzero = [i for i in range(0x28, HEADER_SIZE) if v.header[i]]
    print(f"header 0x28.. {'all zero' if not nonzero else 'nonzero at ' + ', '.join(hex(i) for i in nonzero)}")
    print(f"palette max   {max(v.palette6)} (6-bit VGA)")


def cmd_png(args):
    v = VanFile(args.file)
    os.makedirs(args.outdir, exist_ok=True)
    stop = v.frame_count if args.count is None else min(v.frame_count, args.first + args.count * args.step)
    for i in range(args.first, stop, args.step):
        out = os.path.join(args.outdir, f"frame{i:04d}.png")
        write_png(out, v.width, v.height, v.read_frame(i), v.palette8)
        print(out)


def cmd_slice(args):
    v = VanFile(args.file)
    stop = v.frame_count if args.count is None else min(v.frame_count, args.first + args.count)
    w = VanWriter(args.out, v.width, v.height, v.palette6, v.video_mode, v.variant)
    for i in range(args.first, stop):
        w.write_frame(v.read_frame(i))
    w.close()
    print(f"{args.out}: {stop - args.first} frames")


def _load_inputs(paths):
    """Return (width, height, palette6, [index frames]) from PNG files and/or FLC files."""
    from flc import Flc
    size, frames, pal8 = None, [], None
    indexed_ok = True
    for path in paths:
        if path.lower().endswith((".flc", ".fli")):
            f = Flc(path)
            fsize = (f.width, f.height)
            for n, (pal, pix) in enumerate(f.iter_frames()):
                if n == f.frames:
                    break  # skip the ring (loop-back) frame
                if pal8 is None:
                    pal8 = pal
                if pal != pal8:
                    indexed_ok = False
                frames.append((pal, pix))
        else:
            w, h, rgb = read_png(path)
            fsize = (w, h)
            indexed_ok = False
            frames.append((None, rgb))
        if size and fsize != size:
            raise ValueError(f"{path}: {fsize[0]}x{fsize[1]} differs from {size[0]}x{size[1]}")
        size = fsize
    if not frames:
        raise ValueError("no frames")
    w, h = size
    # FLC frames sharing one palette of exact 6-bit levels: keep the indexes (lossless)
    if indexed_ok and all(c % 4 == 0 for c in pal8):
        return w, h, bytes(c >> 2 for c in pal8), [pix for _, pix in frames]
    rgb_frames = [data if pal is None else b"".join(pal[i * 3:i * 3 + 3] for i in data)
                  for pal, data in frames]
    pal6, idx = quantize(rgb_frames)
    return w, h, pal6, idx


def cmd_make(args):
    w, h, pal6, frames = _load_inputs(args.inputs)
    if args.mode is None:
        mode, exact = pick_mode(w, h)
        if not exact:
            print(f"note: {w}x{h} is not a VP3 mode; using {mode:#x} {VP3_MODES[mode]} (VIEWER centres it)")
    else:
        mode = args.mode
    out = VanWriter(args.out, w, h, pal6, mode, args.variant)
    for f in frames:
        out.write_frame(f)
    out.close()
    print(f"{args.out}: {w}x{h}, {len(frames)} frames, mode {mode:#x}, VANIM{args.variant}")


# 3x5 font for the test pattern
_FONT = {
    "0": "111101101101111", "1": "010110010010111", "2": "111001111100111", "3": "111001111001111",
    "4": "101101111001001", "5": "111100111001111", "6": "111100111101111", "7": "111001001001001",
    "8": "111101111101111", "9": "111101111001111", "T": "111010010010010", "O": "111101101101111",
    "P": "111101111100100", "B": "110101110101110", "M": "101111111101101", "L": "100100100100111",
    "E": "111100110100111", "F": "111100110100100", "R": "110101110101101", "G": "111100101101111",
    "H": "101101111101101", "I": "111010010010111", " ": "000000000000000",
}


def testpattern_palette():
    """Index 0 black, 1 white, 2 red, 3 green, 4 blue, 5 yellow, 6 grey, 7 dark blue-grey,
    8..255 a hue x brightness ramp (8 bands of 31 hues), all 6-bit levels."""
    pal = [(0, 0, 0), (63, 63, 63), (63, 0, 0), (0, 63, 0), (0, 0, 63), (63, 63, 0), (32, 32, 32), (12, 12, 16)]
    for i in range(248):
        hue, val = (i % 31) / 31 * 6, 21 + (i // 31) * 6
        sector, f = int(hue), hue - int(hue)
        q, t = round(val * (1 - f)), round(val * f)
        pal.append([(val, t, 0), (q, val, 0), (0, val, t), (0, q, val), (t, 0, val), (val, 0, q)][sector % 6])
    return bytes(c for rgb in pal for c in rgb)


def testpattern_frame(w, h, n, total):
    """One test frame (top-down indexes).  Shows orientation, palette mapping, edges and frame order."""
    px = bytearray([7]) * (w * h)

    def rect(x0, y0, x1, y1, c):
        x0, y0, x1, y1 = max(0, x0), max(0, y0), min(w, x1), min(h, y1)
        if x1 <= x0:
            return
        for y in range(y0, y1):
            px[y * w + x0:y * w + x1] = bytes([c]) * (x1 - x0)

    def text(x, y, s, c, scale):
        for ch in s:
            g = _FONT[ch]
            for r in range(5):
                for col in range(3):
                    if g[r * 3 + col] == "1":
                        rect(x + col * scale, y + r * scale, x + (col + 1) * scale, y + (r + 1) * scale, c)
            x += 4 * scale

    sc = max(1, h // 100)
    # 1px white border: shows cropping and off-by-one errors
    rect(0, 0, w, 1, 1); rect(0, h - 1, w, h, 1); rect(0, 0, 1, h, 1); rect(w - 1, 0, w, h, 1)
    # orientation: red block top-left, blue block bottom-right, both labelled
    rect(2, 2, 2 + 10 * sc, 2 + 10 * sc, 2)
    text(4 + 10 * sc, 3, "TOP LEFT", 1, sc)
    rect(w - 2 - 10 * sc, h - 2 - 10 * sc, w - 2, h - 2, 4)
    text(w - 4 - 10 * sc - 24 * sc, h - 2 - 6 * sc, "BOTTOM", 1, sc)
    # frame number, large, yellow
    text(w // 2 - 8 * sc, 3, f"F{n:03d}", 5, sc * 2)
    # palette grid: 16x16 cells, cell (i%16, i//16) shows palette index i
    cell = max(2, min((w - 8) // 16, (h - 34 * sc) // 16))
    gx, gy = (w - 16 * cell) // 2, 14 * sc + 2
    for i in range(256):
        x, y = gx + (i % 16) * cell, gy + (i // 16) * cell
        rect(x, y, x + cell - 1, y + cell - 1, i)
    # green bar moving left to right along the bottom over the animation
    bx = 2 + (w - 4 - 8 * sc) * n // max(1, total - 1)
    rect(bx, h - 18 * sc, bx + 8 * sc, h - 13 * sc, 3)
    return bytes(px)


def cmd_testpattern(args):
    w, h = map(int, args.size.lower().split("x"))
    mode = args.mode if args.mode is not None else pick_mode(w, h)[0]
    out = VanWriter(args.out, w, h, testpattern_palette(), mode, args.variant)
    for n in range(args.frames):
        out.write_frame(testpattern_frame(w, h, n, args.frames))
    out.close()
    print(f"{args.out}: {w}x{h}, {args.frames} frames, mode {mode:#x}, VANIM{args.variant}")


def cmd_repair(args):
    v = VanFile(args.file)
    v.f.close()
    with open(args.file, "r+b") as f:
        f.write(b"VANIMWN\0" if args.variant == "WN" else b"VANIMPC\0")
    print(f"{args.file}: magic VANIM{v.variant} -> VANIM{args.variant}")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("info")
    s.add_argument("file")
    s.set_defaults(func=cmd_info)
    s = sub.add_parser("png")
    s.add_argument("file")
    s.add_argument("outdir")
    s.add_argument("--first", type=int, default=0)
    s.add_argument("--count", type=int)
    s.add_argument("--step", type=int, default=1)
    s.set_defaults(func=cmd_png)
    s = sub.add_parser("slice", help="copy a frame range into a new VAN")
    s.add_argument("file")
    s.add_argument("out")
    s.add_argument("--first", type=int, default=0)
    s.add_argument("--count", type=int)
    s.set_defaults(func=cmd_slice)
    mode = lambda v: int(v, 0)
    s = sub.add_parser("make", help="build a VAN from PNG frames and/or FLC files")
    s.add_argument("out")
    s.add_argument("inputs", nargs="+")
    s.add_argument("--mode", type=mode, help="video_mode (default: from size)")
    s.add_argument("--variant", choices=("WN", "PC"), default="WN")
    s.set_defaults(func=cmd_make)
    s = sub.add_parser("testpattern", help="write a synthetic test animation")
    s.add_argument("out")
    s.add_argument("--size", default="320x200")
    s.add_argument("--frames", type=int, default=24)
    s.add_argument("--mode", type=mode)
    s.add_argument("--variant", choices=("WN", "PC"), default="WN")
    s.set_defaults(func=cmd_testpattern)
    s = sub.add_parser("repair", help="rewrite the magic in place, e.g. after VP3 'continue' on a PC file")
    s.add_argument("file")
    s.add_argument("--variant", choices=("WN", "PC"), default="WN")
    s.set_defaults(func=cmd_repair)
    args = p.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    sys.exit(main())
