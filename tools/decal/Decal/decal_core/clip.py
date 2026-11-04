"""Keep-out regions in the mask frame.

Every loop and circle here is already in mask-frame millimetres, u right and v down.
"""

import math

from .raster import PITCH_MM, Mask, _erode_rows, _disc_centred, _from_rows, _to_rows, \
    fill_polygons


def _to_px(loop, pad: int = 0) -> list:
    return [(u / PITCH_MM + pad, v / PITCH_MM + pad) for u, v in loop]


def fill(w: int, h: int, loop: list) -> Mask:
    """Pixels whose centres lie inside one loop, even-odd."""
    return Mask(w, h, fill_polygons(w, h, [_to_px(loop)]))


def allowed(w: int, h: int, outer: list, inners: list, margin_mm: float) -> Mask:
    """Pixels inside outer, outside every inner, and at least margin_mm from either edge.

    The fill runs on a canvas padded by the margin, so an edge just outside the frame still
    trims the pixels beside it.
    """
    pad = int(math.ceil(margin_mm / PITCH_MM)) + 1 if margin_mm > 0 else 0
    cw, ch = w + 2 * pad, h + 2 * pad
    n = cw * ch
    inside = int.from_bytes(fill_polygons(cw, ch, [_to_px(outer, pad)]), "little")
    holes = 0
    for loop in inners:
        holes |= int.from_bytes(fill_polygons(cw, ch, [_to_px(loop, pad)]), "little")
    ones = int.from_bytes(b"\x01" * n, "little")
    canvas = Mask(cw, ch, bytearray((inside & (holes ^ ones)).to_bytes(n, "little")))
    if margin_mm > 0:
        se = _disc_centred(margin_mm / PITCH_MM)
        canvas = _from_rows(_erode_rows(_to_rows(canvas), cw, ch, se), cw, ch)
    if pad == 0:
        return canvas
    out = bytearray()
    for y in range(pad, pad + h):
        out += canvas.bits[y * cw + pad:y * cw + pad + w]
    return Mask(w, h, out)


def clear_circles(mask: Mask, circles: list) -> Mask:
    """Remove art whose pixel centre lies inside any circle (u, v, diameter)."""
    out = mask.copy()
    w, h = mask.w, mask.h
    for u, v, diameter in circles:
        r = diameter / 2.0
        top = max(0, int(math.ceil((v - r) / PITCH_MM - 0.5)))
        bottom = min(h - 1, int(math.floor((v + r) / PITCH_MM - 0.5)))
        for row in range(top, bottom + 1):
            dy = (row + 0.5) * PITCH_MM - v
            span = r * r - dy * dy
            if span < 0:
                continue
            half = math.sqrt(span)
            c0 = max(0, int(math.ceil((u - half) / PITCH_MM - 0.5)))
            c1 = min(w - 1, int(math.floor((u + half) / PITCH_MM - 0.5)))
            if c1 >= c0:
                base = row * w
                out.bits[base + c0:base + c1 + 1] = bytes(c1 - c0 + 1)
    return out
