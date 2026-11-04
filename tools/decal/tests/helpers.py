"""Shared builders for the decal_core tests."""

import math
import os
import struct
import tempfile
import zlib

from decal_core import raster


def chunk(ctype: bytes, body: bytes) -> bytes:
    return (struct.pack(">I", len(body)) + ctype + body
            + struct.pack(">I", zlib.crc32(ctype + body) & 0xFFFFFFFF))


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def pack_row(samples, depth):
    """Pack raw sample values into scanline bytes, most significant bit first."""
    if depth == 8:
        return bytes(samples)
    if depth == 16:
        return b"".join(struct.pack(">H", v) for v in samples)
    per = 8 // depth
    out = bytearray()
    for i in range(0, len(samples), per):
        group = list(samples[i:i + per]) + [0] * (per - len(samples[i:i + per]))
        byte = 0
        for v in group:
            byte = (byte << depth) | v
        out.append(byte)
    return bytes(out)


def filter_row(raw: bytes, prev: bytes, ftype: int, bpp: int) -> bytes:
    out = bytearray()
    for i, x in enumerate(raw):
        a = raw[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        pred = (0, a, b, (a + b) >> 1, _paeth(a, b, c))[ftype]
        out.append((x - pred) & 255)
    return bytes(out)


def png_bytes(w, h, colour_type, depth, rows, palette=None, trns=None, interlace=0,
              cycle_filters=True, extra=()):
    """Build a PNG from rows of raw sample lists. Filter types cycle 0 to 4 down the rows."""
    samples = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour_type]
    bpp = max(1, samples * depth // 8)
    data = bytearray()
    prev = None
    for y, row in enumerate(rows):
        raw = pack_row(row, depth)
        if prev is None:
            prev = bytes(len(raw))
        ftype = y % 5 if cycle_filters else 0
        data.append(ftype)
        data += filter_row(raw, prev, ftype, bpp)
        prev = raw
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, colour_type, 0, 0, interlace))
    if palette is not None:
        out += chunk(b"PLTE", bytes(v for rgb in palette for v in rgb))
    if trns is not None:
        out += chunk(b"tRNS", trns)
    for ctype, body in extra:
        out += chunk(ctype, body)
    out += chunk(b"IDAT", zlib.compress(bytes(data)))
    out += chunk(b"IEND", b"")
    return out


def write_temp(data: bytes, suffix=".png") -> str:
    handle, path = tempfile.mkstemp(suffix=suffix)
    with os.fdopen(handle, "wb") as f:
        f.write(data)
    return path


def luma(r, g, b):
    return (299 * r + 587 * g + 114 * b + 500) // 1000


def random_rows(rng, w, h, per_pixel, depth):
    top = (1 << depth) - 1
    return [[rng.randint(0, top) for _ in range(w * per_pixel)] for _ in range(h)]


def mask_from_text(lines):
    """Build a Mask from strings of '#' (art) and '.' rows."""
    h = len(lines)
    w = len(lines[0])
    bits = bytearray()
    for line in lines:
        bits += bytes(1 if c == "#" else 0 for c in line)
    return raster.Mask(w, h, bits)


def mask_rect(w, h, rects):
    """A mask with art rectangles given as (x0, y0, x1, y1) in pixels, end exclusive."""
    m = raster.Mask(w, h)
    for x0, y0, x1, y1 in rects:
        for y in range(y0, y1):
            m.bits[y * w + x0:y * w + x1] = b"\x01" * (x1 - x0)
    return m


def distance_mm(mask, to_art):
    """Brute-force Euclidean distance in mm from each pixel to the nearest art (or non-art).

    The reference the morphology is checked against. Infinite when the mask has none of the
    target kind.
    """
    target = 1 if to_art else 0
    pts = [(x, y) for y in range(mask.h) for x in range(mask.w)
           if mask.bits[y * mask.w + x] == target]
    if not pts:
        return [math.inf] * (mask.w * mask.h)
    return [min(math.hypot(x - a, y - b) for a, b in pts) * raster.PITCH_MM
            for y in range(mask.h) for x in range(mask.w)]


def shoelace(loop):
    return sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(loop, loop[1:] + loop[:1])) / 2.0


def seg_distance(p, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    len2 = dx * dx + dy * dy
    t = 0.0 if len2 == 0 else max(0.0, min(1.0, ((p[0] - ax) * dx + (p[1] - ay) * dy) / len2))
    return math.hypot(p[0] - (ax + t * dx), p[1] - (ay + t * dy))


def poly_distance(p, loop):
    return min(seg_distance(p, a, b) for a, b in zip(loop, loop[1:] + loop[:1]))


def rect_loop(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


def circle_loop(cx, cy, r, n=360):
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n))
            for i in range(n)]


