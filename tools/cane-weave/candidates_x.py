#!/usr/bin/env python3
"""Candidate lid-face inlay texture, family X: English chintz.

X0 strews loose bouquets over open dark ground, after an English chintz.
Each bouquet is one spray of stems, 25 to 35 mm, carrying three to five
flowers of 6 to 12 mm, every leaf joined to a stem: layered roses, tulips,
six-petal star lilies, morning glories, forget-me-not clusters and
raspberries. Small sprigs sit in the wider gaps. One 96 x 76 mm tile repeats
in half-drop, and its bouquets lean different ways, so no grid shows. The
repeat, print pass and report come from candidates_v.

  candidates_x.py           X0.png
  candidates_x.py --map X0  which motif covers each 1 mm cell of one tile,
                            * where two motifs come within 1 mm; light share before printing
"""

import math
import sys
from functools import partial

import numpy as np

import candidates_b as cb
import candidates_q as cq
import candidates_v as cv
from candidates_b import LIGHT, closing
from candidates_q import Layers, band, leaf, lobes, point_on, polar, soften, turn
from candidates_v import FILLET, LINE, Tile, centre, joined, share, slim_leaf, tulip_head

TULIP_H, TULIP_HW = 8.4, 3.4      # tulip cup height and half-width, mm


# --- Flowers: each paints over what lies beneath, centred on the origin ----------

def tidy(L, region):
  """Drop light slivers under 0.9 mm that halos left inside region."""
  L.light &= ~region | soften(L.light & region)


def rose(L, x, y, R=6.0, phase=-90.0):
  """Front view: five broad outer petals, five shorter petals turned half a
  step laid over them, then a heart of three petals, or a light dot when R
  is under 5.5 mm. Dark divides part the petals of each layer and a dark
  halo rings each layer."""
  layers = [(5, R, R - 1.2, 0.0), (5, R - 2.2, R - 2.8, 36.0)]
  if R >= 5.5:
    layers.append((3, R - 4.4, R - 4.6, 0.0))
  for n, tip, valley, step in layers:
    shape, to_divide, _, _ = lobes(x, y, n, tip, valley, phase=phase + step)
    L.over(shape, soften(shape & (to_divide >= LINE / 2)))
  if R < 5.5:
    centre(L, x, y, 0.5)
  tidy(L, np.hypot(x, y) < R + 0.1)


def star_lily(L, x, y, R=4.3, phase=-90.0):
  """Six pointed petals parted by dark divides round a light dot in a dark ring."""
  shape, to_divide, _, r = lobes(x, y, 6, R, 0.46 * R, phase=phase, point=1.15)
  L.over(shape | (r < 0.46 * R + 0.3), soften(shape & (to_divide >= LINE / 2)))
  centre(L, x, y, 0.55)


def star(x, y, n, r_out, r_in, phase):
  """Regular n-point star, tips r_out from the origin at phase + k * 360 / n
  degrees, inner corners at r_in."""
  step = 2 * math.pi / n
  a = np.radians(np.degrees(np.arctan2(y, x)) - phase)
  a = np.abs((a + step / 2) % step - step / 2)
  r = np.hypot(x, y)
  px, py = r * np.cos(a), r * np.sin(a)
  ex, ey = r_in * math.cos(step / 2) - r_out, r_in * math.sin(step / 2)
  return ex * py - ey * (px - r_out) >= 0


def morning_glory(L, x, y, R=4.4, phase=-90.0):
  """Front view: a round flower whose rim rises to five soft points, a dark
  five-point star in its throat, a light dot in the star."""
  r = np.hypot(x, y)
  a = (np.degrees(np.arctan2(y, x)) - phase - 36) / 72
  off = np.radians(np.abs(a - np.round(a)) * 72)
  rim = 0.7 * R + 0.3 * R * math.cos(math.radians(36)) / np.cos(off)
  shape = soften(r < rim)
  throat = soften(star(x, y, 5, 0.68 * R, 0.3 * R, phase), 0.33)
  L.over(shape, shape & ~throat)
  L.light |= r < 0.5


def forget_me_not(L, x, y, phase=-90.0):
  """Five round petals parted by dark divides round a light dot in a dark
  ring, 5.6 mm across."""
  shape, to_divide, _, r = lobes(x, y, 5, 2.8, 1.75, phase=phase)
  L.over(r < 2.8, soften(shape & (to_divide >= LINE / 2)))
  centre(L, x, y, 0.5)


# Drupelet centres, mm below and beside the top of the berry: hex rows of
# 1.05 mm dots 1.7 mm apart, two, three, two, then one at the tip.

DRUPELETS = [(-0.85, 0.0), (0.85, 0.0), (-1.7, 1.47), (0.0, 1.47), (1.7, 1.47),
             (-0.85, 2.94), (0.85, 2.94), (0.0, 4.41)]


def raspberry(L, x, y):
  """Eight round light drupelets hanging from the origin toward +y."""
  dots = np.zeros(x.shape, bool)
  for dx, dy in DRUPELETS:
    dots |= np.hypot(x - dx, y - 0.525 - dy) < 0.525
  L.over(closing(dots, FILLET), dots)


def rosebud(L, x, y):
  """Closed bud, tip toward -y, a calyx over its base at the origin."""
  cq.bud(L, x, y, 5.5)


# --- Leaves: each returns the blade and its dark rib, or None ------------------

def ribbed_leaf(x, y, base, angle, length, bend=0.0):
  """Pointed oval leaf with a dark midrib."""
  return leaf(x, y, base, angle, length, bend=bend, width=0.42)


def narrow_leaf(x, y, base, angle, length, bend=0.0):
  """Narrow leaf, stem-wide at its foot, no rib."""
  return slim_leaf(x, y, base, angle, length, bend=bend), None


def heart_leaf(x, y, base, angle, length):
  """Heart-shaped leaf on a 1.8 mm stalk from base toward `angle` degrees,
  `length` mm from the tops of its lobes to its tip, a dark midrib."""
  stalk = 1.8
  X, Y = turn(x, y, base[0], base[1], angle)
  k = length / 2.25
  hx, hy = Y / (0.8 * k), 1.0 - (X - stalk) / k
  blade = soften(((hx * hx + hy * hy - 1) ** 3 - hx * hx * hy ** 3 < 0) & (X > stalk - 0.3 * k))
  blade |= (np.abs(Y) < 0.5) & (X > -0.5) & (X < stalk + 0.5)
  rib = (np.abs(Y) < LINE / 2) & (X > stalk + 0.3 * k) & (X < stalk + 1.5 * k)
  return blade, rib


# --- Sprays -------------------------------------------------------------------------

def stem_curves(specs):
  """Bezier control lists from specs: a list of points, or (k, f, points) for
  a stem that starts at fraction f along stem k."""
  curves = []
  for s in specs:
    if isinstance(s[0], int):
      k, f, rest = s
      s = [tuple(point_on(curves[k], f)[0])] + list(rest)
    curves.append(s)
  return curves


def spray(x, y, stems, leaves=(), tulips=(), heads=()):
  """Stems, the leaves on them and tulip cups joined as one light shape, then
  each head painted over it, back to front. Leaves: stem, fraction along it,
  angle, length and leaf shape. Tulips: base and lean. Heads: painter,
  centre and turn."""
  L = Layers(x.shape)
  curves = stem_curves(stems)
  parts, dark = [band(x, y, c) for c in curves], []
  for k, f, angle, length, shape in leaves:
    p, _ = point_on(curves[k], f)
    blade, rib = shape(x, y, tuple(p), angle, length)
    parts.append(blade)
    if rib is not None:
      dark.append(rib)
  for (bx, by), lean in tulips:
    cup, divides = tulip_head(*turn(x, y, bx, by, lean), h=TULIP_H, hw=TULIP_HW)
    parts.append(cup)
    dark.append(divides)
  joined(L, parts, dark)
  for paint, cx, cy, rot in heads:
    paint(L, *turn(x, y, cx, cy, rot))
  return L


def g_spray(x, y, design, flip=1):
  """One spray from its design; flip=-1 mirrors it left to right."""
  return spray(flip * x, y, **design)


def cluster(cx, cy, rot, offsets):
  """Forget-me-nots at offsets round (cx, cy) turned rot degrees: their
  stalks from (cx, cy) and their heads."""
  c, s = math.cos(math.radians(rot)), math.sin(math.radians(rot))
  stalks, heads = [], []
  for ox, oy in offsets:
    px, py = cx + ox * c - oy * s, cy + ox * s + oy * c
    stalks.append([(cx, cy), (px, py)])
    heads.append((forget_me_not, px, py, rot))
  return stalks, heads


def berries(twig, picks):
  """Raspberries on 2 mm stalks off the twig curve: for each (fraction, swing)
  a stalk and a berry hanging below it, swung `swing` degrees."""
  stalks, heads = [], []
  for f, swing in picks:
    p, _ = point_on(twig, f)
    end = p + np.array(polar(2.0, 90 - swing))
    stalks.append([tuple(p), tuple(end)])
    heads.append((raspberry, float(end[0]), float(end[1]), swing))
  return stalks, heads


TRIO = [(0.0, -3.7), (-3.2, 1.85), (3.2, 1.85)]


# Designs in local mm, x right and y down, stems gathered near the bottom.
# Fractions along a stem count from its first point.

# Rose posy: a 12 mm rose, a tulip, a rosebud, a star lily and a
# forget-me-not trio.

_s, _h = cluster(-12.5, 7.5, 0, TRIO)
ROSE_POSY = dict(
  stems=[[(0.5, -6.0), (-0.2, 4.0), (1.0, 15.0)],
         [(-9.0, -3.0), (-6.5, 7.0), (0.0, 15.0)],
         [(9.0, -9.5), (5.5, 2.0), (2.0, 15.0)],
         (2, 0.5, [(8.5, 4.6), (12.0, 3.5)]),
         (1, 0.45, [(-9.0, 6.0), (-12.5, 7.5)])] + _s,
  leaves=[(1, 0.76, 145, 7.5, partial(ribbed_leaf, bend=0.08)),
          (2, 0.78, 22, 7.5, partial(ribbed_leaf, bend=-0.08)),
          (0, 0.55, 195, 5.0, ribbed_leaf),
          (2, 0.3, -25, 5.5, ribbed_leaf)],
  tulips=[((-9.0, -3.0), -20)],
  heads=[(rose, 0.5, -6.0, 8), (rosebud, 9.0, -9.5, 28),
         (partial(star_lily, R=4.3), 12.0, 3.5, 12)] + _h)

# Lilies: three star lilies and a bud up top, three raspberries hung low.

_twig = [(0.29, 9.43), (-6.0, 7.0), (-13.0, 2.5)]
_s, _h = berries(_twig, [(0.2, 8), (0.58, 0), (0.98, -12)])
LILIES = dict(
  stems=[[(0.5, 15.5), (0.0, 5.0), (0.5, -3.0)],
         (0, 1.0, [(-3.5, -3.5), (-7.0, -7.5)]),
         (0, 1.0, [(1.5, -7.0), (2.5, -11.5)]),
         (0, 0.85, [(5.0, -3.0), (10.0, -5.0)]),
         (0, 0.6, [(6.0, 3.5), (12.0, 3.0)]),
         _twig] + _s,
  leaves=[(0, 0.18, 12, 7.5, partial(narrow_leaf, bend=-0.08)),
          (0, 0.4, -12, 7.0, partial(narrow_leaf, bend=-0.1)),
          (0, 0.52, 198, 7.0, partial(narrow_leaf, bend=0.1))],
  heads=[(star_lily, -7.0, -7.5, 5), (star_lily, 2.5, -11.5, 22),
         (star_lily, 10.0, -5.0, 10), (rosebud, 12.0, 3.0, 50)] + _h)

# Morning glories: three flowers and a furled bud on a curving vine with
# heart-shaped leaves.

_vine = [(-15.0, 11.0), (-8.0, -5.0), (3.0, 10.0), (14.0, -7.0)]
GLORIES = dict(
  stems=[_vine,
         (0, 0.2, [(-10.0, 0.0), (-8.5, -5.5)]),
         (0, 0.6, [(2.5, -2.0), (3.5, -6.5)]),
         (0, 0.72, [(6.5, 3.5), (8.5, 7.5)])],
  leaves=[(0, 0.35, 100, 7.0, heart_leaf),
          (0, 0.86, 25, 6.5, heart_leaf)],
  heads=[(morning_glory, -8.5, -5.5, 0), (morning_glory, 3.5, -6.5, 20),
         (morning_glory, 8.5, 7.5, -15), (rosebud, 14.0, -7.0, 40)])

# Tulips: two tulips, a 9.6 mm rose and a forget-me-not trio.

_s, _h = cluster(10.5, 9.5, -10, TRIO)
TULIPS = dict(
  stems=[[(-3.0, -4.5), (-1.5, 6.0), (0.5, 15.0)],
         [(6.5, -1.5), (3.0, 7.0), (1.5, 15.0)],
         [(-9.5, 4.5), (-4.0, 9.0), (0.0, 15.0)],
         (1, 0.6, [(6.5, 9.0), (10.5, 9.5)])] + _s,
  leaves=[(0, 0.5, 232, 8.0, partial(narrow_leaf, bend=0.1)),
          (2, 0.75, 160, 6.5, partial(ribbed_leaf, bend=0.08))],
  tulips=[((-3.0, -4.5), -8), ((6.5, -1.5), 28)],
  heads=[(partial(rose, R=4.8), -9.5, 4.5, -10)] + _h)

# Rose and berries: a 12 mm rose between two rosebuds, two raspberries hung
# off the right-hand stem.

_twig = [(7.72, 2.8), (11.0, 2.0), (14.5, 3.5)]
_s, _h = berries(_twig, [(0.45, 0), (1.0, -15)])
ROSE_BERRIES = dict(
  stems=[[(-1.5, -5.5), (-1.0, 5.0), (0.5, 15.0)],
         [(9.0, -9.0), (9.5, 5.0), (1.5, 15.0)],
         [(-11.5, -7.0), (-11.0, 5.0), (-0.5, 15.0)],
         _twig] + _s,
  leaves=[(0, 0.45, 205, 5.5, ribbed_leaf),
          (2, 0.72, 150, 7.0, partial(ribbed_leaf, bend=0.08)),
          (1, 0.75, 35, 6.5, partial(ribbed_leaf, bend=-0.08)),
          (1, 0.3, -30, 5.0, ribbed_leaf)],
  heads=[(rose, -1.5, -5.5, -15), (rosebud, 9.0, -9.0, 30), (rosebud, -11.5, -7.0, -35)] + _h)

# Sprigs for the wider gaps: a forget-me-not trio, two raspberries, a rosebud.

_s, _h = cluster(0.0, -4.0, 0, TRIO)
FMN_SPRIG = dict(
  stems=[[(0.0, 7.0), (-0.8, 2.0), (0.0, -4.0)]] + _s,
  leaves=[(0, 0.35, 200, 4.5, ribbed_leaf), (0, 0.55, -20, 4.5, ribbed_leaf)],
  heads=_h)

_twig = [(-6.0, 5.0), (-1.0, -2.0), (5.0, -3.0)]
_s, _h = berries(_twig, [(0.55, 10), (1.0, -10)])
BERRY_SPRIG = dict(
  stems=[_twig] + _s,
  leaves=[(0, 0.25, 200, 5.0, ribbed_leaf)],
  heads=_h)

BUD_SPRIG = dict(
  stems=[[(0.0, -1.5), (1.2, 2.5), (-0.5, 6.5)]],
  leaves=[(0, 0.55, 25, 4.8, partial(ribbed_leaf, bend=-0.1))],
  heads=[(rosebud, 0.0, -1.5, 0)])


# --- X0: the tile ---------------------------------------------------------------

# Bouquets (design, tile u, v, turn, flip), five to a tile: the points
# k * (19.2, 38) of the half-drop repeat, rows 38 mm apart and 38.4 mm along
# a row, each nudged up to 6 mm and turned its own way, upright, inverted or
# lying, so the rows do not line up. Sprigs sit in the gaps between them.

X0_TILE = Tile(96.0, 76.0, origin=(2.0, 3.0))
X0_SPRAYS = [(ROSE_POSY, 4.0, 6.0, -12, 1),
             (GLORIES, 22.0, 42.0, 28, 1),
             (LILIES, 42.0, 74.0, 168, 1),
             (TULIPS, 56.0, 34.0, 200, 1),
             (ROSE_BERRIES, 78.0, 8.0, -35, 1),
             (FMN_SPRIG, 68.8, 59.4, -30, 1),
             (BERRY_SPRIG, 29.3, 19.5, 20, 1),
             (BUD_SPRIG, 94.9, 70.8, -40, 1),
             (BUD_SPRIG, 9.0, 57.0, 60, -1)]
REACH = 21.0             # patch half-size round each spray, mm


def x0_placements():
  return [(partial(g_spray, design=d, flip=flip), u, v, rot, REACH)
          for d, u, v, rot, flip in X0_SPRAYS]


def x0_chintz():
  return cv.draw(X0_TILE, x0_placements())


# --- Output -------------------------------------------------------------------

CANDIDATES = [('X0', x0_chintz, X0_TILE, x0_placements)]


def main(args):
  if '--map' in args:
    for name, fn, tile, places in CANDIDATES:
      if name in args:
        cv.tile_map(tile, places(), window=(tile.origin[0], tile.origin[1], tile.w, tile.h))
        print(f'{name}: light {share(fn()):.1f} % of the field before printing')
    return
  cb.OUT.mkdir(parents=True, exist_ok=True)
  for name, fn, _, _ in CANDIDATES:
    if args and name not in args:
      continue
    img, _ = cb.render(name, fn)
    light = np.all(np.asarray(img) == LIGHT, axis=2)
    print(f'  light covers {share(light):.1f} % of the field')


if __name__ == '__main__':
  main(sys.argv[1:])
