#!/usr/bin/env python3
"""Minimal FLI/FLC decoder (stdlib only), used to cross-check VAN2FLC output.

Handles chunk types 4 (COLOR_256), 11 (COLOR_64), 12 (LC), 13 (BLACK),
15 (BRUN), 16 (COPY) and 7 (SS2).  Yields (palette8, pixels) per frame,
including the trailing ring (loop-back) frame.
"""
import struct


def _s8(b):
    return b - 256 if b > 127 else b


class Flc:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        (self.size, self.magic, self.frames, self.width, self.height,
         self.depth) = struct.unpack_from("<IHHHHH", self.data, 0)
        if self.magic not in (0xAF11, 0xAF12):
            raise ValueError(f"bad FLC magic {self.magic:#x}")
        self.speed = struct.unpack_from("<I", self.data, 16)[0]
        self.oframe1, self.oframe2 = struct.unpack_from("<II", self.data, 80)
        if self.magic == 0xAF11 or not self.oframe1:
            self.oframe1 = 128
        self.wide64 = False   # some files hold 8-bit levels in COLOR_64 chunks; see _color

    def iter_frames(self):
        w, h = self.width, self.height
        pix = bytearray(w * h)
        pal = bytearray(768)
        pos = self.oframe1
        d = self.data
        # frames + 1 to include the ring frame when present
        for _ in range(self.frames + 1):
            if pos + 16 > len(d):
                return
            fsize, ftype, nchunks = struct.unpack_from("<IHH", d, pos)
            if ftype != 0xF1FA:
                pos += fsize
                continue
            cpos = pos + 16
            for _ in range(nchunks):
                csize, ctype = struct.unpack_from("<IH", d, cpos)
                body = cpos + 6
                if ctype in (4, 11):
                    self._color(d, body, pal, 4 if ctype == 4 else 11)
                elif ctype == 15:
                    self._brun(d, body, pix, w, h)
                elif ctype == 7:
                    self._ss2(d, body, pix, w)
                elif ctype == 12:
                    self._lc(d, body, pix, w)
                elif ctype == 13:
                    pix[:] = bytes(w * h)
                elif ctype == 16:
                    pix[:] = d[body:body + w * h]
                cpos += csize
            pos += fsize
            yield bytes(pal), bytes(pix)

    def _color(self, d, p, pal, kind):
        # COLOR_64 holds 6-bit levels, but once one chunk has a level above 63 the file's COLOR_64
        # chunks are all read as 8-bit (the player does the same)
        n = struct.unpack_from("<H", d, p)[0]
        p += 2
        idx = 0
        entries = []
        for _ in range(n):
            idx += d[p]
            cnt = d[p + 1] or 256
            p += 2
            for _ in range(cnt):
                entries.append((idx, d[p:p + 3]))
                idx += 1
                p += 3
        if kind == 11 and any(max(rgb) > 63 for _, rgb in entries):
            self.wide64 = True
        for idx, rgb in entries:
            if kind == 11 and not self.wide64:
                rgb = bytes(c << 2 for c in rgb)
            if idx < 256:
                pal[idx * 3:idx * 3 + 3] = rgb

    @staticmethod
    def _brun(d, p, pix, w, h):
        for y in range(h):
            p += 1  # obsolete packet count
            x = 0
            row = y * w
            while x < w:
                c = _s8(d[p]); p += 1
                if c > 0:
                    pix[row + x:row + x + c] = bytes([d[p]]) * c
                    p += 1
                    x += c
                else:
                    c = -c
                    pix[row + x:row + x + c] = d[p:p + c]
                    p += c
                    x += c

    @staticmethod
    def _ss2(d, p, pix, w):
        lines = struct.unpack_from("<H", d, p)[0]
        p += 2
        y = 0
        while lines:
            word = struct.unpack_from("<h", d, p)[0]
            p += 2
            if word < 0:
                if word & 0x4000:
                    y -= word  # skip lines
                else:
                    pix[y * w + w - 1] = word & 0xFF
                continue
            npk = word
            x = 0
            row = y * w
            for _ in range(npk):
                x += d[p]
                c = _s8(d[p + 1])
                p += 2
                if c > 0:
                    pix[row + x:row + x + 2 * c] = d[p:p + 2 * c]
                    p += 2 * c
                    x += 2 * c
                else:
                    c = -c
                    pix[row + x:row + x + 2 * c] = d[p:p + 2] * c
                    p += 2
                    x += 2 * c
            y += 1
            lines -= 1

    @staticmethod
    def _lc(d, p, pix, w):
        y, lines = struct.unpack_from("<HH", d, p)
        p += 4
        for _ in range(lines):
            npk = d[p]; p += 1
            x = 0
            row = y * w
            for _ in range(npk):
                x += d[p]
                c = _s8(d[p + 1])
                p += 2
                if c > 0:
                    pix[row + x:row + x + c] = d[p:p + c]
                    p += c
                    x += c
                else:
                    c = -c
                    pix[row + x:row + x + c] = bytes([d[p]]) * c
                    p += 1
                    x += c
            y += 1
