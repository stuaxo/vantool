#!/usr/bin/env python3
"""Test-only FLI/FLC encoder (stdlib only) that can use every chunk type vanplay reads.

vantool flc only writes BRUN, SS2 and LC, so this exists to give the player's decoder input with
COPY and BLACK frames, partial palette changes, SS2 line skips and odd-width last-pixel words, LC
inside an FLC, and a chunk type it must skip.

  frames: list of bytes (width * height palette indexes), shown in order
  plan:   list with one entry per frame, plus one for the ring frame (back to frame 0); each is a
          dict: {"kind": "brun"|"copy"|"black"|"ss2"|"lc", "pal": [chunk, ...]} where a palette
          chunk is ("256"|"64", start, [(r, g, b), ...]) with levels 0..255 or 0..63
"""
import struct


def _chunk(ctype, body):
    body = bytes(body)
    if len(body) & 1:
        body += b"\0"
    return struct.pack("<IH", 6 + len(body), ctype) + body


def _color(kind, start, rgb):
    """One palette chunk: colours start .. start+len(rgb)-1 (all 256 from 0 use a count of 0)."""
    assert len(rgb) <= 255 or (start == 0 and len(rgb) == 256)
    count = 0 if len(rgb) == 256 else len(rgb)
    body = struct.pack("<H", 1) + bytes([start, count]) + b"".join(bytes(c) for c in rgb)
    return _chunk(4 if kind == "256" else 11, body)


def _brun(pix, w, h):
    out = bytearray()
    for y in range(h):
        row = pix[y * w:(y + 1) * w]
        out.append(0)                       # obsolete packet count
        x = 0
        while x < w:
            r = 1
            while x + r < w and r < 127 and row[x + r] == row[x]:
                r += 1
            if r >= 3:
                out += bytes([r, row[x]])
                x += r
            else:
                n = 1
                while x + n < w and n < 127 and not (x + n + 2 < w and row[x + n] == row[x + n + 1] == row[x + n + 2]):
                    n += 1
                out += bytes([256 - n]) + row[x:x + n]
                x += n
    return _chunk(15, out)


def _packets(a, b, unit, lit_pos):
    """Packets for the changed parts of two rows (units of 1 or 2 bytes): skip byte, signed count,
    data.  A positive count copies; a negative one repeats (LC) -- SS2 is the same way round."""
    n = len(a) // unit
    same = [a[i * unit:(i + 1) * unit] == b[i * unit:(i + 1) * unit] for i in range(n)]
    out = bytearray()
    count = 0
    x = 0                                    # unit position after the last packet
    i = 0
    while i < n:
        if same[i]:
            i += 1
            continue
        s = i                                # region start: extend over short unchanged gaps
        e = i + 1
        j = e
        while j < n and j - e < 3:
            if not same[j]:
                e = j + 1
            j += 1
        gap = (s - x) * unit
        while gap > 255:                     # a skip byte holds at most 255
            step = 254 // unit * unit
            out += bytes([step, 1]) + b[(x + step // unit) * unit:(x + step // unit + 1) * unit]
            x += step // unit + 1
            gap = (s - x) * unit
            count += 1
        p = s
        while p < e:
            r = 1
            while p + r < e and r < 127 and b[(p + r) * unit:(p + r + 1) * unit] == b[p * unit:(p + 1) * unit]:
                r += 1
            skip = (p - x) * unit
            if r >= 3:
                out += bytes([skip, (256 - r)]) + b[p * unit:(p + 1) * unit]
                p += r
            else:
                m = 1
                while p + m < e and m < 127 and not (p + m + 2 < e and b[(p + m) * unit:(p + m + 1) * unit] ==
                                                     b[(p + m + 1) * unit:(p + m + 2) * unit] ==
                                                     b[(p + m + 2) * unit:(p + m + 3) * unit]):
                    m += 1
                out += bytes([skip, m]) + b[p * unit:(p + m) * unit]
                p += m
            x = p
            count += 1
        i = e
    return bytes(out), count


def _lc(prev, cur, w, h):
    changed = [y for y in range(h) if prev[y * w:(y + 1) * w] != cur[y * w:(y + 1) * w]]
    if not changed:
        return None
    first, last = changed[0], changed[-1]
    out = struct.pack("<HH", first, last - first + 1)
    for y in range(first, last + 1):
        data, count = _packets(prev[y * w:(y + 1) * w], cur[y * w:(y + 1) * w], 1, True)
        assert count < 256, "too many LC packets in a line"
        out += bytes([count]) + data
    return _chunk(12, out)


def _ss2(prev, cur, w, h):
    body = bytearray()
    lines = 0
    skipped = 0
    for y in range(h):
        a, b = prev[y * w:(y + 1) * w], cur[y * w:(y + 1) * w]
        if a == b:
            skipped += 1
            continue
        while skipped:
            n = min(skipped, 16384)
            body += struct.pack("<H", 0x10000 - n)
            skipped -= n
        nw = w // 2
        data, count = _packets(a[:nw * 2], b[:nw * 2], 2, True)
        if w & 1 and a[-1] != b[-1]:
            body += struct.pack("<H", 0x8000 | b[-1])
        body += struct.pack("<H", count) + data
        lines += 1
    return _chunk(7, struct.pack("<H", lines) + bytes(body)) if lines else None


def build(path, width, height, frames, plan, fli=False, delay=100):
    """Write the animation.  delay: milliseconds per frame (FLC) -- FLI stores it in 1/70 s."""
    assert len(plan) == len(frames) + 1
    pic = [bytes(width * height)] + list(frames)       # pic[0]: the blank screen before frame 0
    out = bytearray(128)
    offsets = []
    for i, step in enumerate(plan):
        cur = frames[i] if i < len(frames) else frames[0]
        prev = pic[i] if i else pic[0]
        if i == len(frames):
            prev = frames[-1]                           # ring frame: last -> first
        chunks = [_color(k, s, rgb) for (k, s, rgb) in step.get("pal", [])]
        if step.get("stamp"):
            chunks.append(_chunk(18, b"\1\2\3\4\5"))     # postage stamp: must be skipped
        kind = step["kind"]
        if kind == "brun":
            chunks.append(_brun(cur, width, height))
        elif kind == "copy":
            chunks.append(_chunk(16, cur))
        elif kind == "black":
            chunks.append(_chunk(13, b""))
        else:
            c = (_lc if kind == "lc" else _ss2)(prev, cur, width, height)
            if c:
                chunks.append(c)
        body = b"".join(chunks)
        offsets.append(len(out))
        out += struct.pack("<IHH8x", 16 + len(body), 0xF1FA, len(chunks)) + body
    struct.pack_into("<IHHHHHH", out, 0, len(out), 0xAF11 if fli else 0xAF12, len(frames), width, height, 8,
                     0 if fli else 3)
    if fli:
        struct.pack_into("<H", out, 16, max(1, round(delay * 70 / 1000)))
    else:
        struct.pack_into("<I", out, 16, delay)
        struct.pack_into("<II", out, 80, 128, offsets[1] if len(offsets) > 1 else 0)
    open(path, "wb").write(out)
