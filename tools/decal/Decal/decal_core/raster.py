"""Raster masks at 0.1 mm per pixel and the operations on them.

The mask frame is image-like. Pixel (col, row) has its centre at u = (col + 0.5) * PITCH_MM and
v = (row + 0.5) * PITCH_MM, with v growing downward.

Rows are held as Python integers for the morphology, so a shift or an OR touches a whole row
in C. Resampling and blurring run on prefix sums per row, so their cost does not depend on
the window size.
"""

import array
import math
import re
from bisect import bisect_right
from itertools import accumulate

from .errors import DecalError

PITCH_MM = 0.1

# A source pixel wider than this on the face makes edges look stepped.
COARSE_SOURCE_MM = 0.3

# Shading mode scales (mm), the lighting blur and the relief blur.
LIGHTING_MM = 8.0
SHAPE_MM = 2.5
RELIEF_GAIN = 230.0

_TO_ASCII = bytes.maketrans(b"\x00\x01", b"01")
_FROM_ASCII = bytes.maketrans(b"01", b"\x00\x01")
_FLIP01 = bytes.maketrans(b"\x00\x01", b"\x01\x00")
_RUN = re.compile(rb"\x01+")


def _sub(tick, lo: float, hi: float):
    """Map a loop's 0..1 progress onto the lo..hi slice of a caller's range."""
    if tick is None:
        return None
    span = hi - lo
    return lambda f: tick(lo + span * f)


class Mask:
    """A w by h bitmap. bits holds one byte per pixel, 0 or 1, and 1 is art."""

    def __init__(self, w: int, h: int, bits: bytearray = None, warnings: list = None):
        self.w = w
        self.h = h
        self.bits = bits if bits is not None else bytearray(w * h)
        self.warnings = warnings if warnings is not None else []
        if len(self.bits) != w * h:
            raise ValueError(f"mask bits hold {len(self.bits)} bytes, expected {w * h}")

    def copy(self) -> "Mask":
        return Mask(self.w, self.h, bytearray(self.bits), list(self.warnings))

    def count(self) -> int:
        """Number of art pixels."""
        return self.bits.count(1)


# ---------------------------------------------------------------------------------------
# Row integers

def _to_rows(mask: Mask) -> list:
    """Return one integer per row, bit x set when column x is art."""
    w = mask.w
    bits = mask.bits
    return [int(bytes(bits[y * w:(y + 1) * w]).translate(_TO_ASCII)[::-1], 2)
            for y in range(mask.h)]


def _from_rows(rows: list, w: int, h: int, warnings: list = None) -> Mask:
    out = bytearray()
    for r in rows:
        out += format(r, f"0{w}b")[::-1].encode().translate(_FROM_ASCII)
    return Mask(w, h, out, list(warnings or []))


def _seg_or(r: int, a: int, b: int) -> int:
    """OR of r shifted left by every k in a..b (right when k is negative)."""
    length = b - a + 1
    out = r
    cover = 1
    while cover * 2 <= length:
        out |= out << cover
        cover *= 2
    if cover < length:
        out |= out << (length - cover)
    return out << a if a >= 0 else out >> -a


def _seg_and(p: int, a: int, b: int) -> int:
    """AND of p shifted right by every k in a..b (left when k is negative)."""
    length = b - a + 1
    out = p
    cover = 1
    while cover * 2 <= length:
        out &= out >> cover
        cover *= 2
    if cover < length:
        out &= out >> (length - cover)
    return out >> a if a >= 0 else out << -a


def _disc_centred(radius_px: float) -> list:
    """Structuring element (dy, a, b) of every integer offset within radius_px of the origin."""
    reach = int(math.floor(radius_px + 1e-9))
    out = []
    for dy in range(-reach, reach + 1):
        half = int(math.floor(math.sqrt(max(0.0, radius_px * radius_px - dy * dy)) + 1e-9))
        out.append((dy, -half, half))
    return out


def _disc_pixels(diameter_px: float) -> list:
    """Structuring element of the pixels whose centres lie in a disc of this diameter.

    The element is anchored at its top-left corner. An opening does not depend on the anchor,
    and this form keeps its axis extent at ceil(diameter), so a bar exactly as wide as the
    diameter survives and anything narrower does not.
    """
    extent = max(1, int(math.ceil(diameter_px - 1e-9)))
    centre = diameter_px / 2.0
    out = []
    for j in range(extent):
        rem = centre * centre - (j + 0.5 - centre) ** 2
        if rem < 0:
            continue
        half = math.sqrt(rem)
        lo = int(math.ceil(centre - half - 0.5 - 1e-9))
        hi = int(math.floor(centre + half - 0.5 + 1e-9))
        if hi >= lo:
            out.append((j, lo, hi))
    return out


def _by_segment(se: list) -> dict:
    groups = {}
    for dy, a, b in se:
        groups.setdefault((a, b), []).append(dy)
    return groups


def _erode_rows(rows: list, w: int, h: int, se: list, tick=None) -> list:
    """Pixels p where every p + s of the element is art. Outside the frame counts as art."""
    reach = max(max(abs(a), abs(b)) for _, a, b in se)
    pad = (1 << reach) - 1
    padded = [(pad << (reach + w)) | (r << reach) | pad for r in rows]
    full = (1 << w) - 1
    out = [full] * h
    steps = len(se) + len(_by_segment(se))
    done = 0
    for (a, b), dys in _by_segment(se).items():
        shifted = [(_seg_and(p, a, b) >> reach) & full for p in padded]
        done += 1
        if tick:
            tick(done / steps)
        for dy in dys:
            for y in range(max(0, -dy), min(h, h - dy)):
                out[y] &= shifted[y + dy]
            done += 1
            if tick:
                tick(done / steps)
    return out


def _dilate_rows(rows: list, w: int, h: int, se: list, tick=None) -> list:
    """Union of the element placed on every art pixel. Nothing grows beyond the frame."""
    full = (1 << w) - 1
    out = [0] * h
    steps = len(se) + len(_by_segment(se))
    done = 0
    for (a, b), dys in _by_segment(se).items():
        shifted = [_seg_or(r, a, b) & full for r in rows]
        done += 1
        if tick:
            tick(done / steps)
        for dy in dys:
            for y in range(max(0, dy), min(h, h + dy)):
                out[y] |= shifted[y - dy]
            done += 1
            if tick:
                tick(done / steps)
    return out


def _open_rows(rows: list, w: int, h: int, se: list, outside: int, tick=None) -> list:
    """Opening with an element anchored at its corner.

    The frame grows by the element's reach on every side, filled with `outside` (0 or 1), so
    translates that start outside the frame still count.
    """
    m = max(max(dy for dy, _, _ in se), max(abs(v) for _, a, b in se for v in (a, b))) + 1
    ext_w = w + 2 * m
    edge = ((1 << m) - 1) if outside else 0
    full_ext = ((1 << ext_w) - 1) if outside else 0
    ext = [full_ext] * m
    ext += [(edge << (m + w)) | (r << m) | edge for r in rows]
    ext += [full_ext] * m
    ext = _erode_rows(ext, ext_w, h + 2 * m, se, _sub(tick, 0.0, 0.5))
    ext = _dilate_rows(ext, ext_w, h + 2 * m, se, _sub(tick, 0.5, 1.0))
    full = (1 << w) - 1
    return [(r >> m) & full for r in ext[m:m + h]]


def erode(mask: Mask, radius_mm: float) -> Mask:
    """Remove art within radius_mm of non-art. The frame edge does not erode."""
    if radius_mm <= 0:
        return mask.copy()
    se = _disc_centred(radius_mm / PITCH_MM)
    return _from_rows(_erode_rows(_to_rows(mask), mask.w, mask.h, se), mask.w, mask.h,
                      mask.warnings)


def dilate(mask: Mask, radius_mm: float) -> Mask:
    """Add every pixel within radius_mm of art."""
    if radius_mm <= 0:
        return mask.copy()
    se = _disc_centred(radius_mm / PITCH_MM)
    return _from_rows(_dilate_rows(_to_rows(mask), mask.w, mask.h, se), mask.w, mask.h,
                      mask.warnings)


# ---------------------------------------------------------------------------------------
# Components

class _Labels:
    """Run-length labelling of a mask."""

    def __init__(self, starts, ends, labels, count):
        self.starts = starts
        self.ends = ends
        self.labels = labels
        self.count = count

    def at(self, x: int, y: int) -> int:
        """Component label of art pixel (x, y), or -1 for non-art."""
        starts = self.starts[y]
        i = bisect_right(starts, x) - 1
        if i >= 0 and self.ends[y][i] > x:
            return self.labels[y][i]
        return -1


def label(mask: Mask, eight: bool, tick=None) -> _Labels:
    """Label connected art components, 8-connected or 4-connected."""
    w = mask.w
    bits = mask.bits
    slack = 1 if eight else 0
    parent = []

    def find(i):
        root = i
        while parent[root] != root:
            root = parent[root]
        while parent[i] != root:
            parent[i], i = root, parent[i]
        return root

    starts_rows, ends_rows, ids_rows = [], [], []
    prev_s, prev_e, prev_i = [], [], []
    for y in range(mask.h):
        spans = [m.span() for m in _RUN.finditer(bits[y * w:(y + 1) * w])]
        cur_s = [s for s, _ in spans]
        cur_e = [e for _, e in spans]
        cur_i = list(range(len(parent), len(parent) + len(spans)))
        parent.extend(cur_i)
        i = j = 0
        while i < len(prev_s) and j < len(cur_s):
            if cur_s[j] < prev_e[i] + slack and prev_s[i] < cur_e[j] + slack:
                a, b = find(prev_i[i]), find(cur_i[j])
                if a != b:
                    parent[b] = a
            if prev_e[i] <= cur_e[j]:
                i += 1
            else:
                j += 1
        starts_rows.append(cur_s)
        ends_rows.append(cur_e)
        ids_rows.append(cur_i)
        prev_s, prev_e, prev_i = cur_s, cur_e, cur_i
        if tick:
            tick((y + 1) / mask.h)

    roots = {}
    labels_rows = []
    for ids in ids_rows:
        row = []
        for i in ids:
            root = find(i)
            row.append(roots.setdefault(root, len(roots)))
        labels_rows.append(row)
    return _Labels(starts_rows, ends_rows, labels_rows, len(roots))


def count_pieces(mask: Mask, tick=None) -> int:
    """Number of 8-connected art components."""
    return label(mask, True, tick).count


# ---------------------------------------------------------------------------------------
# Resample and blur

def _resample(data, w: int, h: int, tw: int, th: int, tick=None) -> list:
    """Box-filter a w by h byte image to tw by th. Returns th rows of floats (array 'd').

    Each output pixel is the exact area average of the source it covers, so up- and
    down-scaling share one path.
    """
    sx = w / tw
    sy = h / th
    edges = [i * sx for i in range(tw + 1)]
    floors = [min(int(e), w) for e in edges]
    fracs = [e - f for e, f in zip(edges, floors)]
    inv_x = 1.0 / sx
    cache = {}

    def source_row(k):
        got = cache.get(k)
        if got is None:
            if tw == w:
                got = [float(v) for v in data[k * w:(k + 1) * w]]
            else:
                row = bytes(data[k * w:(k + 1) * w]) + b"\x00"
                pre = list(accumulate(row[:-1], initial=0))
                cum = [pre[f] + fr * row[f] for f, fr in zip(floors, fracs)]
                got = [(b - a) * inv_x for a, b in zip(cum, cum[1:])]
            cache[k] = got
        return got

    out = []
    for j in range(th):
        y0 = j * sy
        y1 = min((j + 1) * sy, float(h))
        k0 = min(int(y0), h - 1)
        k1 = min(int(math.ceil(y1 - 1e-9)) - 1, h - 1)
        acc = None
        for k in range(k0, max(k0, k1) + 1):
            weight = (min(y1, k + 1) - max(y0, k)) / sy
            if weight <= 1e-12:
                continue
            row = source_row(k)
            if acc is None:
                acc = [v * weight for v in row]
            else:
                acc = [a + v * weight for a, v in zip(acc, row)]
        out.append(array.array("d", acc if acc is not None else source_row(k0)))
        for stale in [k for k in cache if k < int((j + 1) * sy)]:
            del cache[stale]
        if tick:
            tick((j + 1) / th)
    return out


def _box_h(rows: list, radius: int, tick=None) -> list:
    inv = 1.0 / (2 * radius + 1)
    width = 2 * radius + 1
    out = []
    for y, row in enumerate(rows):
        ext = [row[0]] * radius + list(row) + [row[-1]] * radius
        pre = list(accumulate(ext, initial=0.0))
        out.append(array.array("d", [(late - early) * inv
                                     for late, early in zip(pre[width:], pre)]))
        if tick:
            tick((y + 1) / len(rows))
    return out


def _box_v(rows: list, radius: int, tick=None) -> list:
    h = len(rows)
    inv = 1.0 / (2 * radius + 1)
    run = [0.0] * len(rows[0])
    for j in range(-radius, radius + 1):
        run = [a + b for a, b in zip(run, rows[min(max(j, 0), h - 1)])]
    out = []
    for y in range(h):
        out.append(array.array("d", [v * inv for v in run]))
        add = rows[min(y + radius + 1, h - 1)]
        sub = rows[max(y - radius, 0)]
        run = [v + a - b for v, a, b in zip(run, add, sub)]
        if tick:
            tick((y + 1) / h)
    return out


def _blur(rows: list, sigma_mm: float, tick=None) -> list:
    """Three box passes in each direction, matching a Gaussian of sigma_mm."""
    sigma = sigma_mm / PITCH_MM
    radius = max(1, int(round((math.sqrt(4.0 * sigma * sigma + 1.0) - 1.0) / 2.0)))
    for i in range(3):
        rows = _box_h(rows, radius, _sub(tick, i / 3, (i + 0.5) / 3))
        rows = _box_v(rows, radius, _sub(tick, (i + 0.5) / 3, (i + 1) / 3))
    return rows


def _threshold_rows(rows: list, limit: float, art_below: bool) -> bytearray:
    """Row floats to 0/1 bytes. art_below marks values <= limit, else values >= limit."""
    out = bytearray()
    cmp = limit.__ge__ if art_below else limit.__le__
    for row in rows:
        out += bytearray(map(cmp, row))
    return out


def from_grey(w, h, grey, alpha, width_mm, mode, threshold, invert, tick=None) -> Mask:
    """Resample an 8-bit image to the face scale and threshold it into art.

    mode 'dark' marks grey <= threshold. mode 'shading' removes the lighting by dividing by an
    8 mm blur, measures the relief against a 2.5 mm blur, and marks relief <= -threshold.
    invert swaps art and background. Pixels under 50 % alpha are never art.
    tick, when given, receives the fraction done, 0 to 1.
    """
    if mode not in ("dark", "shading"):
        raise DecalError(f"The trace mode {mode!r} is unknown. Use Dark or Shading.")
    if len(grey) != w * h:
        raise DecalError(f"The image data holds {len(grey)} samples for {w} by {h} pixels.")
    tw = max(1, int(round(width_mm / PITCH_MM)))
    th = max(1, int(round(tw * h / w)))
    warnings = []
    source_mm = width_mm / w
    if source_mm > COARSE_SOURCE_MM:
        warnings.append(
            f"The image is {w} pixels wide on a {width_mm:.1f} mm face, so each source pixel is "
            f"{source_mm:.2f} mm wide and the edges will look stepped.")

    has_alpha = alpha is not None
    if mode == "dark":
        rows = _resample(grey, w, h, tw, th, _sub(tick, 0.0, 0.8 if has_alpha else 1.0))
    else:
        rows = _resample(grey, w, h, tw, th, _sub(tick, 0.0, 0.15))
    lo = min(min(r) for r in rows)
    hi = max(max(r) for r in rows)
    if mode == "dark":
        bits = _threshold_rows(rows, float(threshold), True)
    else:
        lit = _blur(rows, LIGHTING_MM, _sub(tick, 0.15, 0.5))
        flat = [array.array("d", [g / max(b, 1.0) for g, b in zip(gr, br)])
                for gr, br in zip(rows, lit)]
        shape = _blur(flat, SHAPE_MM, _sub(tick, 0.5, 0.85))
        relief = [array.array("d", [(f - s) * RELIEF_GAIN for f, s in zip(fr, sr)])
                  for fr, sr in zip(flat, shape)]
        bits = _threshold_rows(relief, -float(threshold), True)
    if invert:
        bits = bits.translate(_FLIP01)
    if alpha is not None:
        opaque = _threshold_rows(
            _resample(alpha, w, h, tw, th, _sub(tick, 0.8 if mode == "dark" else 0.9, 1.0)),
            128.0, False)
        bits = bytearray((int.from_bytes(bits, "little") & int.from_bytes(opaque, "little"))
                         .to_bytes(tw * th, "little"))

    art = bits.count(1)
    if art == 0 or art == tw * th:
        raise DecalError(
            f"The image is all one colour after the threshold of {threshold}. Its grey values "
            f"run from {lo:.0f} to {hi:.0f}. Move the threshold inside that range or change "
            "the trace mode.")
    return Mask(tw, th, bits, warnings)


# ---------------------------------------------------------------------------------------
# Loops

def fill_polygons(w: int, h: int, loops: list) -> bytearray:
    """Even-odd scanline fill of loops given in pixel units. Returns w*h bytes of 0 or 1.

    A pixel is inside when its centre is.
    """
    bits = bytearray(w * h)
    crossings = [[] for _ in range(h)]
    for loop in loops:
        n = len(loop)
        for i in range(n):
            x1, y1 = loop[i]
            x2, y2 = loop[(i + 1) % n]
            if y1 == y2:
                continue
            if y1 > y2:
                x1, y1, x2, y2 = x2, y2, x1, y1
            r0 = max(0, int(math.ceil(y1 - 0.5)))
            r1 = min(h - 1, int(math.ceil(y2 - 0.5)) - 1)
            if r0 > r1:
                continue
            slope = (x2 - x1) / (y2 - y1)
            for r in range(r0, r1 + 1):
                crossings[r].append(x1 + (r + 0.5 - y1) * slope)
    ones = b"\x01" * w
    for r in range(h):
        xs = crossings[r]
        if len(xs) < 2:
            continue
        xs.sort()
        base = r * w
        for i in range(0, len(xs) - 1, 2):
            c0 = max(0, int(math.ceil(xs[i] - 0.5)))
            c1 = min(w, int(math.ceil(xs[i + 1] - 0.5)))
            if c1 > c0:
                bits[base + c0:base + c1] = ones[:c1 - c0]
    return bits


def from_loops(loops: list, width_mm: float) -> Mask:
    """Rasterise closed loops, scaled so their bounding box is width_mm wide.

    The loops use y up, as a sketch does. The mask is image-like, so the rows run top to
    bottom and the art keeps its orientation.
    """
    points = [p for loop in loops for p in loop]
    if not points:
        raise DecalError("The shape has no closed loop to fill.")
    min_x = min(p[0] for p in points)
    max_x = max(p[0] for p in points)
    min_y = min(p[1] for p in points)
    max_y = max(p[1] for p in points)
    if max_x - min_x <= 0 or max_y - min_y <= 0:
        raise DecalError("The shape has no width or no height, so there is nothing to fill.")
    w = max(1, int(round(width_mm / PITCH_MM)))
    scale = w / (max_x - min_x)
    h = max(1, int(round((max_y - min_y) * scale)))
    px = [[((x - min_x) * scale, (max_y - y) * scale) for x, y in loop] for loop in loops]
    return Mask(w, h, fill_polygons(w, h, px))


# ---------------------------------------------------------------------------------------
# Orientation

def rotate(mask: Mask, quarter_turns: int) -> Mask:
    """Rotate clockwise on screen by quarter_turns of 90 degrees."""
    turns = quarter_turns % 4
    w, h, bits = mask.w, mask.h, mask.bits
    if turns == 0:
        return mask.copy()
    if turns == 2:
        return Mask(w, h, bytearray(bits[::-1]), list(mask.warnings))
    out = bytearray()
    if turns == 1:
        for x in range(w):
            out += bits[x::w][::-1]
    else:
        for x in range(w - 1, -1, -1):
            out += bits[x::w]
    return Mask(h, w, out, list(mask.warnings))


def mirror(mask: Mask) -> Mask:
    """Flip left to right."""
    w = mask.w
    out = bytearray()
    for y in range(mask.h):
        out += mask.bits[y * w:(y + 1) * w][::-1]
    return Mask(w, mask.h, out, list(mask.warnings))


def intersect(a: Mask, b: Mask) -> Mask:
    """Pixels that are art in both masks."""
    if (a.w, a.h) != (b.w, b.h):
        raise ValueError(f"mask sizes differ, {a.w}x{a.h} against {b.w}x{b.h}")
    n = a.w * a.h
    bits = (int.from_bytes(a.bits, "little") & int.from_bytes(b.bits, "little")).to_bytes(
        n, "little")
    return Mask(a.w, a.h, bytearray(bits), list(a.warnings))


# ---------------------------------------------------------------------------------------
# Width

def open_close(mask: Mask, min_width_mm: float, tick=None):
    """Remove pieces and gaps narrower than min_width_mm.

    An opening with a disc of that diameter removes pieces and bridges thinner than it, and
    a closing fills gaps thinner than it. A piece or gap exactly min_width_mm wide stays.
    Art ends at the frame edge, so a sliver there goes like any other, and convex corners
    round off at min_width_mm / 2.
    Returns the new mask and the pieces lost (8-connected before minus after, never negative).
    tick, when given, receives the fraction done, 0 to 1.
    """
    diameter = min_width_mm / PITCH_MM
    if diameter <= 1.0 or mask.count() == 0:
        return mask.copy(), 0
    w, h = mask.w, mask.h
    full = (1 << w) - 1
    se = _disc_pixels(diameter)
    before = count_pieces(mask, _sub(tick, 0.0, 0.15))
    rows = _open_rows(_to_rows(mask), w, h, se, 0, _sub(tick, 0.15, 0.5))
    # closing is the opening of the background; beyond the frame the background continues
    inverse = _open_rows([full ^ r for r in rows], w, h, se, 1, _sub(tick, 0.5, 0.85))
    result = _from_rows([full ^ r for r in inverse], w, h, mask.warnings)
    lost = max(0, before - count_pieces(result, _sub(tick, 0.85, 1.0)))
    return result, lost
