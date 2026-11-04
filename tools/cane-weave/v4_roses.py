#!/usr/bin/env python3
"""Rose icons for V4, the mixed ditsy in candidates_v.

Five roses, each in V4's icon interface: a function of local mm (x right,
y down, origin at the icon's centre) that returns its Layers, so any one can
take the bell's place in V4_KINDS. Each is drawn to read as a flower first:
separate petals round a centre, or a bloom on a stem. The top views are up
to 12 mm across and the side view 12.9 mm tall. A petal laid over another
clears a 0.7 mm dark halo in it, dark lines are 0.65 mm, and each rose's
light is softened so the print pass finds little to change.

  v4_roses.py           the sheet build/cane/candidates/V4-roses.png and each
                        rose's print-pass change
  v4_roses.py --draft   the five printed icons at 4x in /tmp/v4-roses-draft.png
"""

import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

import candidates_b as cb
import candidates_v as cv
from candidates_b import ALLOWED, DARK, LIGHT, S
from candidates_q import Layers, band, bezier, ellipse, leaf, lobes, point_on, polar, soften
from candidates_v import LINE, centre, joined, stroke


# --- Shared parts ---------------------------------------------------------------

def outline(x, y, curves):
  """Pixels inside the closed outline made of the Bezier curves on each
  control list in turn, each starting where the one before it ends."""
  pts = np.vstack([bezier(c)[0] for c in curves])
  inside = np.zeros(x.shape, bool)
  for (x0, y0), (x1, y1) in zip(pts, np.roll(pts, -1, axis=0)):
    if y0 == y1:
      continue
    crosses = (y0 > y) != (y1 > y)
    inside ^= crosses & (x < x0 + (y - y0) * (x1 - x0) / (y1 - y0))
  return inside


def mirrored(curves):
  """A right half outline from bottom centre to top centre, closed with its
  mirror image down the left side."""
  back = [[(-px, py) for px, py in reversed(c)] for c in reversed(curves)]
  return curves + back

def petal_ring(L, x, y, n, tip, valley, phase):
  """n round petals out to `tip` and in to `valley`, parted by dark divides,
  laid over what is there."""
  shape, to_divide, _, _ = lobes(x, y, n, tip, valley, phase=phase)
  L.over(shape, soften(shape & (to_divide >= LINE / 2)))


def layered(L, x, y, R):
  """Five broad round petals out to R, five smaller ones laid over them
  turned 36 degrees so their tips sit in the outer notches, and a light
  centre in a dark ring."""
  petal_ring(L, x, y, 5, R, 0.66 * R, -90)
  petal_ring(L, x, y, 5, 0.52 * R, 0.36 * R, -54)
  centre(L, x, y, 0.15 * R)


# --- Roses ----------------------------------------------------------------------

def g_rose_layered(x, y):
  """RF, top view, 11 mm: the layered rose."""
  L = Layers(x.shape)
  layered(L, x, y, 5.5)
  L.light = soften(L.light)
  return L


def g_rose_cabbage(x, y):
  """RG, cabbage rose, top view, 12 mm: five outer petals, then two rows of
  cupped petals, each petal laid over the row outside it and over its
  neighbour so it shows as a separate crescent, and a small round bud in
  the middle."""
  L = Layers(x.shape)
  petal_ring(L, x, y, 5, 6.0, 4.8, -90)
  for n, d, half_w, half_l, phase in [(5, 1.95, 2.1, 2.15, -54), (2, 0.85, 1.7, 1.55, 0)]:
    for k in range(n):
      a = phase + 360 * k / n
      p = ellipse(x, y, *polar(d, a), half_w, half_l, angle=a - 90)
      L.over(p)
  L.over(np.hypot(x, y) < 0.7)
  L.light = soften(L.light)
  return L


# The side-view bloom's right half, bottom centre to the bud's top: the bowl,
# the rim curling out to a petal tip, back in along the curl to the notch,
# up round the bud.

BLOOM = mirrored([[(0.0, 0.0), (0.9, 0.0), (2.9, -0.9), (3.4, -3.0)],
                  [(3.4, -3.0), (3.4, -3.8), (3.8, -4.2), (4.1, -4.6)],
                  [(4.1, -4.6), (3.3, -4.75), (2.2, -4.7), (1.45, -3.9)],
                  [(1.45, -3.9), (1.75, -4.9), (1.0, -5.9), (0.0, -5.95)]])


def g_rose_side(x, y):
  """RH, side view, upright, 12.9 mm tall: a cup whose rim curls out into a
  petal tip each side of a round bud, one dark line along the front petal's
  rim from the left notch round under the bud, so the petal wraps it, two
  sepals laid over the foot of the cup curving up its sides, and a stem
  with one leaf."""
  L = Layers(x.shape)
  y = y + 0.6
  stem = [(0.0, -0.5), (0.6, 3.0), (0.0, 6.4)]
  p, _ = point_on(stem, 0.75)
  joined(L, [band(x, y, stem), leaf(x, y, tuple(p), -160, 4.2, bend=0.1, width=0.42)[0]])
  bloom = outline(x, y, BLOOM)
  L.over(bloom, bloom & ~stroke(x, y, [(-1.75, -4.2), (-0.6, -1.9), (0.9, -2.2), (0.9, -3.3)]))
  for side in (-1, 1):
    L.over(leaf(x, y, (side * 0.2, 0.4), -90 + side * 55, 3.0, bend=side * 0.12, width=0.42)[0])
  L.join(band(x, y, [(0.0, 0.4), (0.0, 1.6)]))
  L.light = soften(L.light)
  return L


def g_rose_tattoo(x, y):
  """RI, tattoo rose head, three-quarter view, 10.8 mm: the tips of three
  back petals rise above a tight bud with one dark seam, and two large front
  petals cup it from below, their rims rising toward the outside and the
  right laid over the left. Each petal is laid over the ones behind it."""
  L = Layers(x.shape)
  y = y + 0.2
  for cx, cy, a, b in [(0.0, -3.4, 2.2, 1.8), (-3.2, -2.3, 2.0, 1.9), (3.2, -2.3, 2.0, 1.9)]:
    L.over(ellipse(x, y, cx, cy, a, b))
  bud = ellipse(x, y, 0.0, -1.3, 2.0, 1.7)
  L.over(bud, bud & ~stroke(x, y, [(-0.5, -2.9), (0.5, -2.5), (0.3, -1.9)]))
  front = ellipse(x, y, 0.0, 0.3, 5.4, 4.2) & (y > 0.2 - 0.07 * x * x)
  L.over(front & (x < 0.5))
  L.over(front & (np.hypot(x - 3.5, y - 0.6) < 4.2))
  L.light = soften(L.light)
  return L


def g_rose_leaves(x, y):
  """RJ, the layered rose at 9 mm with a leaf tucked under each side, at
  opposite corners so the whole stays 11 mm square."""
  L = Layers(x.shape)
  for a in (225, 45):
    L.over(leaf(x, y, polar(3.4, a), a, 4.4, bend=0.08, width=0.38)[0])
  layered(L, x, y, 4.5)
  L.light = soften(L.light)
  return L


# Name, what it is, function, and how far it may turn either way in V4 (the bell's 22 degrees for
# a side view, any angle for a top view).

ROSES = [('RF', 'layered rose, top view', g_rose_layered, 180),
         ('RG', 'cabbage rose, top view', g_rose_cabbage, 180),
         ('RH', 'rose on a stem, side view', g_rose_side, 22),
         ('RI', 'tattoo rose head', g_rose_tattoo, 30),
         ('RJ', 'layered rose with leaves', g_rose_leaves, 180)]


# --- Print pass and V4 context ------------------------------------------------------

ICON = 7.0                    # half the side of an icon's square, mm
STAMP = (15.0, 40.0)          # face v, u mm where an icon takes the print pass, clear of every hole
PATCH = (127.0, 3.0, 48.0)    # face u, v and side mm of the V4 patch: one tile's area


def icon_grid():
  c = (np.arange(int(2 * ICON * S)) + 0.5) / S - ICON
  return np.meshgrid(c, c)


def printed_icon(fn):
  """The icon after the print pass, and the share of its light pixels the pass changed."""
  x, y = icon_grid()
  light = fn(x, y).light
  assert not (light[[0, -1], :].any() or light[:, [0, -1]].any()), 'icon overruns its square'
  n = light.shape[0]
  r0, c0 = int(STAMP[0] * S), int(STAMP[1] * S)
  window = (slice(r0, r0 + n), slice(c0, c0 + n))
  assert ALLOWED[window].all(), 'stamp window meets a hole or the field edge'
  face = np.zeros(ALLOWED.shape, bool)
  face[window] = light
  out, changed = cb.printable(face)
  return out[window], changed


def v4_with(fn, swing):
  """V4's face with the rose in every bell's place, its turn scaled from the bell's swing to its own."""
  bell_swing = next(s for g, _, s in cv.V4_KINDS if g is cv.g_bell)
  places = [(fn, u, v, rot * swing / bell_swing, reach) if g is cv.g_bell else (g, u, v, rot, reach)
            for g, u, v, rot, reach in cv.v4_placements()]
  return cv.draw(cv.V4_TILE, places)


# --- Sheet ----------------------------------------------------------------------

def tone(mask):
  img = np.empty(mask.shape + (3,), np.uint8)
  img[:] = DARK
  img[mask] = LIGHT
  return Image.fromarray(img)


def font(size):
  try:
    return ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial Bold.ttf', size)
  except OSError:
    return ImageFont.load_default()


def sheet(rows):
  n, pad, side = int(2 * ICON * S), 20, round(PATCH[2] * S)
  col, y4 = 4 * n + 2 * pad, 100 + n + 40
  yp = y4 + 30 + 4 * n + 20
  img = Image.new('RGB', (col * len(rows), yp + 30 + side + 20), (255, 255, 255))
  pen = ImageDraw.Draw(img)
  big, small = font(30), font(20)
  for k, (name, what, changed, icon, patch) in enumerate(rows):
    x0 = k * col + pad
    pen.text((x0, 20), f'{name}  {what}', fill=(0, 0, 0), font=big)
    pen.text((x0, 62), f'print pass changed {changed * 100:.2f} % of light', fill=(70, 70, 70), font=small)
    pen.text((x0, 100), '1x', fill=(70, 70, 70), font=small)
    img.paste(tone(icon), (x0 + 40, 100))
    pen.text((x0, y4), '4x', fill=(70, 70, 70), font=small)
    img.paste(tone(icon).resize((4 * n, 4 * n), Image.NEAREST), (x0, y4 + 30))
    pen.text((x0, yp), 'in V4 at 1x, the bell replaced (48 mm)', fill=(70, 70, 70), font=small)
    img.paste(patch, (x0, yp + 30))
  return img


def main(args):
  rows = []
  for name, what, fn, swing in ROSES:
    icon, changed = printed_icon(fn)
    print(f'{name}: print pass changed {changed * 100:.2f} % of light pixels')
    patch = None
    if '--draft' not in args:
      light, _ = cb.printable(v4_with(fn, swing))
      u, v, side = PATCH
      patch = cb.face_image(light).crop((round(u * S), round(v * S), round((u + side) * S),
                                         round((v + side) * S)))
    rows.append((name, what, changed, icon, patch))
  if '--draft' in args:
    n = 4 * int(2 * ICON * S)
    strip = Image.new('RGB', (n * len(rows), n), (255, 255, 255))
    for k, row in enumerate(rows):
      strip.paste(tone(row[3]).resize((n, n), Image.NEAREST), (k * n, 0))
    strip.save('/tmp/v4-roses-draft.png')
    return
  cb.OUT.mkdir(parents=True, exist_ok=True)
  out = cb.OUT / 'V4-roses.png'
  sheet(rows).save(out)
  print(out)


if __name__ == '__main__':
  main(sys.argv[1:])
