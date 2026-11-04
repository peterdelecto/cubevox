"""Mask to polygons by crack following along pixel edges.

The edges between art and non-art pixels join into closed loops whose areas are exact. The
loops are then thinned with Douglas-Peucker.

Loops are in mask-frame millimetres, u right and v down. Outer loops run counter-clockwise on
screen and holes clockwise. plan.py flips v to y up, which makes outer loops positive in the
usual signed-area sense and holes negative.
"""

from dataclasses import dataclass, field

from .raster import PITCH_MM, Mask, _sub, _to_rows, label


@dataclass
class Piece:
    """One connected piece of art, its outer loop and the holes inside it."""

    outer: list
    holes: list = field(default_factory=list)
    area_mm2: float = 0.0


def _set_bits(value: int):
    """Indices of the set bits of a non-negative integer."""
    if not value:
        return
    text = bin(value)[:1:-1]
    i = text.find("1")
    while i >= 0:
        yield i
        i = text.find("1", i + 1)


def _edges(mask: Mask, tick=None):
    """Directed boundary edges with art on the left, as {start: end} plus a second table.

    Vertices are numbered y * (w + 1) + x on the pixel corners. The second table holds the
    extra outgoing edge at a vertex where two art pixels meet only at a corner.
    """
    w, h = mask.w, mask.h
    stride = w + 1
    rows = _to_rows(mask)
    first = {}
    second = {}

    def add(a, b):
        if a in first:
            second[a] = b
        else:
            first[a] = b

    for y in range(h + 1):
        above = rows[y - 1] if y > 0 else 0
        below = rows[y] if y < h else 0
        base = y * stride
        for x in _set_bits(above & ~below):
            add(base + x, base + x + 1)
        for x in _set_bits(below & ~above):
            add(base + x + 1, base + x)
        if tick:
            tick(0.5 * (y + 1) / (h + 1))
    for y in range(h):
        r = rows[y]
        top = y * stride
        for x in _set_bits(r & ~(r << 1)):
            add(top + x, top + x + stride)
        for x in _set_bits((r << 1) & ~r):
            add(top + stride + x, top + x)
        if tick:
            tick(0.5 + 0.5 * (y + 1) / h)
    return first, second, stride


# Vertices walked between progress reports inside one loop.
_HOOK_EVERY = 16384


def _follow(start: int, first: dict, second: dict, stride: int, hook=None):
    """Walk one loop from start.

    Returns its corner vertex ids, its first edge's art pixel and the edges walked. hook, when
    given, receives the edges walked so far every _HOOK_EVERY vertices.
    """
    left_of = {1: -stride, -stride: -1, -1: stride, stride: 1}
    nxt = first.pop(start)
    if start in second:
        first[start] = second.pop(start)
    first_diff = nxt - start
    sy, sx = divmod(start, stride)
    if first_diff == 1:
        art = (sx, sy - 1)
    elif first_diff == -1:
        art = (sx - 1, sy)
    elif first_diff == stride:
        art = (sx, sy)
    else:
        art = (sx - 1, sy - 1)
    corners = []
    last = first_diff
    v = nxt
    steps = 1
    while v != start:
        steps += 1
        if hook and steps % _HOOK_EVERY == 0:
            hook(steps)
        if v in second:
            a = first[v]
            if a - v == left_of[last]:
                n = a
                first[v] = second.pop(v)
            else:
                n = second.pop(v)
        else:
            n = first.pop(v)
        diff = n - v
        if diff != last:
            corners.append(v)
            last = diff
        v = n
    if last != first_diff:
        corners.append(start)
    return corners, art, steps


def _simplify_closed(pts: list, tol: float) -> list:
    """Douglas-Peucker on a closed loop. Every dropped point stays within tol of the result."""
    n = len(pts)
    if n <= 3 or tol <= 0:
        return pts
    x0, y0 = pts[0]
    far = max(range(1, n), key=lambda i: (pts[i][0] - x0) ** 2 + (pts[i][1] - y0) ** 2)
    xs = [p[0] for p in pts] + [x0]
    ys = [p[1] for p in pts] + [y0]
    keep = [False] * n
    keep[0] = keep[far] = True
    tol2 = tol * tol
    stack = [(0, far), (far, n)]
    while stack:
        i, j = stack.pop()
        if j - i < 2:
            continue
        ax, ay, bx, by = xs[i], ys[i], xs[j], ys[j]
        dx, dy = bx - ax, by - ay
        len2 = dx * dx + dy * dy
        worst = -1.0
        at = i
        for k in range(i + 1, j):
            px = xs[k] - ax
            py = ys[k] - ay
            t = (px * dx + py * dy) / len2 if len2 else 0.0
            if t <= 0.0:
                d2 = px * px + py * py
            elif t >= 1.0:
                ex = xs[k] - bx
                ey = ys[k] - by
                d2 = ex * ex + ey * ey
            else:
                cross = px * dy - py * dx
                d2 = cross * cross / len2
            if d2 > worst:
                worst = d2
                at = k
        if worst > tol2:
            keep[at] = True
            stack.append((i, at))
            stack.append((at, j))
    return [p for p, k in zip(pts, keep) if k]


def _area2(pts: list) -> int:
    """Twice the signed shoelace area."""
    return sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(pts, pts[1:] + pts[:1]))


def loops(mask: Mask, simplify_mm: float, min_area_mm2: float, tick=None):
    """Trace the mask into pieces.

    Loops under min_area_mm2 are dropped, and a dropped outer loop takes its holes along. The
    second value counts the loops dropped for size, including loops that simplify away.
    Outer loops and holes are thinned to simplify_mm. Piece.area_mm2 is exact, from the pixel
    edges, with the kept holes taken out.
    tick, when given, receives the fraction done, 0 to 1.
    """
    first, second, stride = _edges(mask, _sub(tick, 0.0, 0.25))
    total = max(1, len(first) + len(second))
    walked = 0
    follow_tick = _sub(tick, 0.25, 0.55)

    def walk(start):
        nonlocal walked
        base = walked
        hook = None if tick is None else (lambda s: follow_tick(min(1.0, (base + s) / total)))
        corners, art, steps = _follow(start, first, second, stride, hook)
        walked += steps
        if tick:
            follow_tick(min(1.0, walked / total))
        return corners, art

    found = []
    starts = [v for v in first if v not in second]
    for start in starts:
        if start in first and start not in second:
            found.append(walk(start))
    while first:
        found.append(walk(next(iter(first))))

    outers = []
    holes = []
    for corners, art in found:
        pts = [(v % stride, v // stride) for v in corners]
        a2 = _area2(pts)
        if a2 < 0:
            outers.append((pts, -a2 / 2.0, art))
        else:
            holes.append((pts, a2 / 2.0, art))

    pixel_mm2 = PITCH_MM * PITCH_MM
    owner = {}
    if holes:
        comps = label(mask, False, _sub(tick, 0.55, 0.65))
        for i, (_, _, art) in enumerate(outers):
            owner[comps.at(*art)] = i
        owned = [[] for _ in outers]
        for hole in holes:
            owned[owner[comps.at(*hole[2])]].append(hole)
    else:
        owned = [[] for _ in outers]

    tol_px = simplify_mm / PITCH_MM
    dropped = 0
    pieces = []
    for index, ((pts, area, _), its_holes) in enumerate(zip(outers, owned)):
        if tick:
            tick(0.65 + 0.35 * index / len(outers))
        if area * pixel_mm2 < min_area_mm2:
            dropped += 1
            continue
        outer = _simplify_closed(pts, tol_px)
        if len(outer) < 3:
            dropped += 1
            continue
        kept = []
        area_mm2 = area * pixel_mm2
        for hpts, harea, _ in its_holes:
            if harea * pixel_mm2 < min_area_mm2:
                dropped += 1
                continue
            thin = _simplify_closed(hpts, tol_px)
            if len(thin) < 3:
                dropped += 1
                continue
            kept.append([(x * PITCH_MM, y * PITCH_MM) for x, y in thin])
            area_mm2 -= harea * pixel_mm2
        pieces.append(Piece([(x * PITCH_MM, y * PITCH_MM) for x, y in outer], kept, area_mm2))
    pieces.sort(key=lambda p: (min(q[1] for q in p.outer), min(q[0] for q in p.outer)))
    return pieces, dropped


def points(pieces: list) -> int:
    """Total vertices in every outer loop and hole."""
    return sum(len(p.outer) + sum(len(h) for h in p.holes) for p in pieces)
