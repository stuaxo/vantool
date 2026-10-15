#!/usr/bin/env python3
"""xwd (ZPixmap, 24/32bpp) on stdin -> PNG file argv[1]; optional crop x,y,w,h argv[2]."""
import sys, struct, zlib
d = sys.stdin.buffer.read()
h = struct.unpack('>25I', d[:100])
hsize, ver, fmt, depth, w, ht = h[0], h[1], h[2], h[3], h[4], h[5]
bpp, bpl, ncolors = h[11], h[12], h[19]
off = hsize + ncolors * 12
px = d[off:]
crop = list(map(int, sys.argv[2].split(','))) if len(sys.argv) > 2 else [0, 0, w, ht]
cx, cy, cw, ch = crop
raw = bytearray()
for y in range(cy, cy + ch):
    raw.append(0)
    row = px[y * bpl:(y + 1) * bpl]
    for x in range(cx, cx + cw):
        o = x * (bpp // 8)
        b, g, r = row[o], row[o + 1], row[o + 2]
        raw += bytes((r, g, b))
def chunk(t, p):
    return struct.pack('>I', len(p)) + t + p + struct.pack('>I', zlib.crc32(t + p) & 0xffffffff)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', cw, ch, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b''))
