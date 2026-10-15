#!/usr/bin/env python3
"""Compare a screen capture of VIEWER/VP3 playback against the frames of a VAN.

vancheck.py FILE.VAN SCREEN.png X Y SCALE
  X,Y   top-left of the image on the capture (in capture pixels)
  SCALE integer display scale (e.g. 2 for mode 13h shown at 640x400)
Prints the best-matching frame and the share of pixels whose colour is exactly
what the VGA DAC would show ((v<<2)|(v>>4) per 6-bit channel).
"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from vanfile import VanFile
from imgio import read_png

van, shot, x0, y0, sc = sys.argv[1], sys.argv[2], *map(int, sys.argv[3:6])
v = VanFile(van)
sw, sh, rgb = read_png(shot)
pal = [tuple((c << 2) | (c >> 4) for c in v.palette6[i * 3:i * 3 + 3]) for i in range(256)]
screen = []
for y in range(v.height):
    for x in range(v.width):
        o = ((y0 + y * sc + sc // 2) * sw + (x0 + x * sc + sc // 2)) * 3
        screen.append(tuple(rgb[o:o + 3]))
best = max(((sum(pal[p] == s for p, s in zip(v.read_frame(i), screen)), i) for i in range(v.frame_count)))
n = v.width * v.height
print(f"best frame {best[1]}: {best[0]}/{n} pixels exact ({100 * best[0] / n:.2f}%)")
sys.exit(0 if best[0] == n else 1)
