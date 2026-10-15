"""Small stdlib-only image helpers for the VAN prototype tools.

read_png   -> (width, height, rgb_bytes)       any non-interlaced 8-bit-or-less PNG
write_png  -> 8-bit palettized PNG
quantize   -> shared palette + index frames for a list of RGB frames
"""
import struct
import zlib


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path):
    """Return (width, height, rgb) with rgb as width*height*3 bytes (alpha dropped)."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, plte = 8, bytearray(), None
    while pos < len(data):
        n, tag = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif tag == b"PLTE":
            plte = body
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
        pos += 12 + n
    if interlace:
        raise ValueError(f"{path}: interlaced PNG not supported")
    if depth == 16:
        raise ValueError(f"{path}: 16-bit PNG not supported")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bits = depth * channels
    stride = (w * bits + 7) // 8
    bpp = max(1, bits // 8)
    raw = zlib.decompress(bytes(idat))
    prev = bytearray(stride)
    rows = []
    p = 0
    for _ in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 0xFF
            elif f == 2:
                line[i] = (line[i] + b) & 0xFF
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif f == 4:
                line[i] = (line[i] + _paeth(a, b, c)) & 0xFF
        rows.append(line)
        prev = line
    out = bytearray()
    for line in rows:
        if depth < 8:
            per = 8 // depth
            mask = (1 << depth) - 1
            vals = [(line[x // per] >> (8 - depth * (x % per + 1))) & mask for x in range(w)]
            if ctype == 0:
                vals = [v * 255 // mask for v in vals]
        else:
            vals = None
        for x in range(w):
            if ctype == 3:
                i = vals[x] if vals else line[x]
                out += plte[i * 3:i * 3 + 3]
            elif ctype in (0, 4):
                g = vals[x] if vals else line[x * channels]
                out += bytes((g, g, g))
            else:
                o = x * channels
                out += line[o:o + 3]
    return w, h, bytes(out)


def write_png(path, width, height, pixels, palette8):
    """Write an 8-bit palettized PNG."""
    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += pixels[y * width:(y + 1) * width]
    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n")
        out.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)))
        out.write(chunk(b"PLTE", palette8))
        out.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        out.write(chunk(b"IEND", b""))


def _to6(rgb):
    """8-bit RGB triple -> packed 18-bit key of 6-bit VGA levels."""
    return ((rgb[0] >> 2) << 12) | ((rgb[1] >> 2) << 6) | (rgb[2] >> 2)


def quantize(frames, ncolors=256):
    """frames: list of RGB byte strings.  Returns (palette6, [index frames]).

    Colours are reduced to 6-bit VGA levels first, because that is all a VAN can
    store.  If the result fits in ncolors it is exact; otherwise a median cut over
    the colour histogram of all frames builds one shared palette.
    """
    hist = {}
    keyed = []
    for rgb in frames:
        keys = [_to6(rgb[i:i + 3]) for i in range(0, len(rgb), 3)]
        keyed.append(keys)
        for k in keys:
            hist[k] = hist.get(k, 0) + 1
    colors = sorted(hist)
    if len(colors) <= ncolors:
        lut = {k: i for i, k in enumerate(colors)}
        pal = colors
    else:
        pal, lut = _median_cut(hist, ncolors)
    palette6 = bytearray(768)
    for i, k in enumerate(pal):
        palette6[i * 3:i * 3 + 3] = bytes(((k >> 12) & 63, (k >> 6) & 63, k & 63))
    return bytes(palette6), [bytes(lut[k] for k in keys) for keys in keyed]


def _median_cut(hist, ncolors):
    boxes = [list(hist)]
    def chan(k, c):
        return (k >> (12 - 6 * c)) & 63
    while len(boxes) < ncolors:
        # split the box with the largest weighted channel range
        best, bi, bc = -1, None, 0
        for i, box in enumerate(boxes):
            if len(box) < 2:
                continue
            for c in range(3):
                vals = [chan(k, c) for k in box]
                r = (max(vals) - min(vals)) * sum(hist[k] for k in box)
                if r > best:
                    best, bi, bc = r, i, c
        if bi is None:
            break
        box = sorted(boxes.pop(bi), key=lambda k: chan(k, bc))
        total = sum(hist[k] for k in box)
        acc, cut = 0, 1
        for j, k in enumerate(box):
            acc += hist[k]
            if acc >= total / 2:
                cut = max(1, min(len(box) - 1, j + 1))
                break
        boxes += [box[:cut], box[cut:]]
    pal, lut = [], {}
    for i, box in enumerate(boxes):
        w = sum(hist[k] for k in box)
        avg = [round(sum(chan(k, c) * hist[k] for k in box) / w) for c in range(3)]
        pal.append((avg[0] << 12) | (avg[1] << 6) | avg[2])
        for k in box:
            lut[k] = i
    return pal, lut
