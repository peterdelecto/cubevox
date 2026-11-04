#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family W: three classic patterns from
the owner's reference sheet, one per panel.

  W0  folk flower scatter: eight-, six- and five-petal flowers on a half-drop
      grid, so each kind runs in diagonal rows, with small leaf sprigs in
      the gaps.
  W1  tile grid: square tiles holding a flower medallion alternate with round
      mandala flowers; scroll ornaments and small squares sit in the gaps.
  W2  paisley with flowers: paisleys of two sizes, an outline band round a
      petal ring or a dot row, with small round flowers and dots between.

Each candidate repeats one tile of placed motifs in half-drop, painted back
to front with the tools of candidates_v; the face grid, print pass and
report come from candidates_b.

  candidates_w.py           every candidate as <name>.png
  candidates_w.py W0 W2     only the named candidates
  candidates_w.py --map W0  which motif covers each 1 mm cell of a 60 mm window,
                            * where two motifs come within 1 mm; light share before printing
"""

import math
import sys
from functools import lru_cache, partial

import numpy as np

import candidates_b as cb
from candidates_b import LIGHT, disc, opening
from candidates_o import polygon_sdf
from candidates_q import Layers, band, bezier, capsule, leaf, lobes, point_on, soften, turn
from candidates_v import LINE, RING, STEM, Tile, centre, draw, joined, share, tile_map, to_rays


# --- Shared shapes --------------------------------------------------------------

def spoon(X, Y, r0, r1, base_r, tip_r):
  """Signed distance to a petal along +X from r0 to r1: a base circle base_r
  round and a tip circle tip_r round joined by their tangents."""
  h = (r1 - tip_r) - (r0 + base_r)
  b = (base_r - tip_r) / h
  a = math.sqrt(1 - b * b)
  along, across = X - (r0 + base_r), np.abs(Y)
  k = -b * across + a * along
  return np.where(k < 0, np.hypot(across, along) - base_r,
                  np.where(k > a * h, np.hypot(across, along - h) - tip_r,
                           a * across + b * along - base_r))


def spoon_flower(L, x, y, n, R, disc_r, tip_r, base_r=0.5, dot=0.0, phase=-90.0):
  """n separate petals, round and broad at the tip, round a light centre disc
  in a dark ring; at least LINE of dark between neighbours."""
  r0 = disc_r + RING + 0.05
  petals = np.zeros(x.shape, bool)
  for k in range(n):
    X, Y = turn(x, y, 0, 0, phase + 360 * k / n)
    petals |= spoon(X, Y, r0, R, base_r, tip_r) < 0
  petals &= to_rays(x, y, n, phase + 180 / n) >= LINE / 2
  L.over(np.hypot(x, y) < R, soften(petals))
  centre(L, x, y, disc_r, dot)


def blossom5(L, x, y, R, phase=-90.0):
  """Five round petals parted by dark divides round a light centre in a dark ring."""
  shape, to_divide, _, r = lobes(x, y, 5, R, 0.62 * R, phase=phase)
  L.over(r < R, soften(shape & (to_divide >= LINE / 2)))
  centre(L, x, y, 0.75)


def dot(x, y, d=1.3):
  L = Layers(x.shape)
  L.over(np.hypot(x, y) < d / 2)
  return L


# --- W0: folk flower scatter ------------------------------------------------------

def g_flower8(x, y):
  """Eight broad round-tipped petals, 12 mm."""
  L = Layers(x.shape)
  spoon_flower(L, x, y, 8, 6.0, 1.1, 1.35)
  return L


def g_flower6(x, y):
  """Six broad round-tipped petals round a centre with a dark dot, 11 mm."""
  L = Layers(x.shape)
  spoon_flower(L, x, y, 6, 5.5, 1.2, 1.5, base_r=0.55, dot=0.7)
  return L


def g_flower5(x, y):
  """Five broad round-tipped petals, 9.5 mm."""
  L = Layers(x.shape)
  spoon_flower(L, x, y, 5, 4.75, 0.8, 1.45)
  return L


SPRIG_STEM = [(0.0, 0.0), (1.6, -0.5), (3.2, 0.0)]


def g_sprig(x, y, n=3):
  """Two or three leaves on a gently curved stem along +x, the stem foot at
  the origin: a leaf at the tip, and one leaf, or one each side, leaning
  forward off the stem."""
  L = Layers(x.shape)
  parts = [band(x, y, SPRIG_STEM)]
  sides = [(0.3, -1), (0.62, 1)] if n == 3 else [(0.45, 1)]
  p, head = point_on(SPRIG_STEM, 0.97)
  blade, _ = leaf(x, y, tuple(p), head, 3.8, width=0.46)
  parts.append(blade)
  for f, side in sides:
    p, head = point_on(SPRIG_STEM, f)
    blade, _ = leaf(x, y, tuple(p), head + 42 * side, 3.4, bend=0.06 * side, width=0.46)
    parts.append(blade)
  joined(L, parts)
  return L


# Flowers on the lattice A = (15, 8.5), B = (0, 17): neighbours 17 mm apart.
# Kind (i - j) mod 3 differs between every pair of neighbours, so each kind
# runs in diagonal rows, and each triangle of neighbours holds one of each.
# The half-drop repeat (45, 25.5) is 3A; the tile holds nine flowers. One
# sprig sits in each triangle, its leaves toward the wide middle of the gap
# and its stem toward the largest flower.

W0_A, W0_B = np.array([15.0, 8.5]), np.array([0.0, 17.0])
W0_TILE = Tile(45.0, 51.0, origin=(2.0, 3.0))
W0_KINDS = [(g_flower8, 6.0), (g_flower6, 5.5), (g_flower5, 4.75)]


def w0_point(i, j):
  return i * W0_A + j * W0_B, (i - j) % 3


def w0_placements():
  sprigs, flowers = [], []
  for i in range(3):
    for j in range(3):
      p, kind = w0_point(i, j)
      group, R = W0_KINDS[kind]
      flowers.append((group, float(p[0]), float(p[1]), 360 * cb.hash01(i, j, 1), R + 1.0))
      for n, tri in enumerate(([(i, j), (i + 1, j), (i, j + 1)], [(i + 1, j), (i, j + 1), (i + 1, j + 1)])):
        pts = [w0_point(a, b) for a, b in tri]
        mid = np.mean([q for q, _ in pts], axis=0)
        big = next(q for q, k in pts if k == 0)
        d = (mid - big) / np.linalg.norm(mid - big)
        foot = mid - 3.4 * d
        sprigs.append((partial(g_sprig, n=3 - n), float(foot[0]), float(foot[1]),
                       math.degrees(math.atan2(d[1], d[0])), 7.0))
  return sprigs + flowers


def w0_folk():
  return draw(W0_TILE, w0_placements())


# --- W1: tile grid ------------------------------------------------------------------

def g_square(x, y, half=6.25):
  """Light square tile with a dark frame line inside its edge and a dark
  flower in the middle: four broad petals on the diagonals, four short ones
  on the axes, round a light centre disc in a dark ring."""
  L = Layers(x.shape)
  ax, ay = np.abs(x), np.abs(y)
  tile = soften((ax < half) & (ay < half), 0.6)
  inner = soften((ax < half - 0.9) & (ay < half - 0.9), 0.3)
  field = soften((ax < half - 1.55) & (ay < half - 1.55), 0.45)
  dark = np.hypot(x, y) < 1.0 + RING
  for k in range(8):
    X, Y = turn(x, y, 0, 0, 45 * k)
    if k % 2:
      dark |= spoon(X, Y, 1.2, 4.7, 0.4, 0.95) < 0
    else:
      dark |= spoon(X, Y, 1.2, 3.3, 0.35, 0.6) < 0
  dark &= (to_rays(x, y, 8, 22.5) >= 0.45) | (np.hypot(x, y) < 1.0 + RING)
  light = (tile & ~inner) | soften(field & ~dark)
  light |= np.hypot(x, y) < 1.0
  L.over(tile, light)
  return L


def g_mandala(x, y, R=6.5):
  """Round flower: twelve scalloped outer petals parted by dark divides, a
  dark ring, eight pointed inner petals, and a light centre in a dark ring."""
  L = Layers(x.shape)
  r = np.hypot(x, y)
  outer, to_div, _, _ = lobes(x, y, 12, R, R - 1.5)
  inner, to_div8, _, _ = lobes(x, y, 8, 3.9, 2.6, phase=22.5, point=1.0)
  petals = soften(outer & (to_div >= LINE / 2) & (r >= 4.55))
  petals |= soften(inner & (to_div8 >= LINE / 2) & (r >= 1.0 + RING + 0.05))
  L.over(r < R, petals)
  centre(L, x, y, 1.0)
  return L


def g_scroll(x, y):
  """Scroll ornament along local x: a small light diamond, and on each side
  a short stem forking into two curls that roll back toward the middle,
  each ending in a round eye."""
  L = Layers(x.shape)
  parts = [soften(np.abs(x) + np.abs(y) < 1.4, 0.3)]
  for sx in (-1, 1):
    parts.append(capsule(x, y, (sx * 1.0, 0.0), (sx * 2.3, 0.0), 0.9))
    for sy in (-1, 1):
      ctrl = [(sx * 2.1, 0.0), (sx * 3.9, sy * 0.1), (sx * 4.3, sy * 2.1), (sx * 2.9, sy * 2.5)]
      parts.append(band(x, y, ctrl, 0.9))
      parts.append(np.hypot(x - sx * 2.9, y - sy * 2.5) < 0.65)
  joined(L, parts)
  return L


def g_small_square(x, y, side=2.2):
  L = Layers(x.shape)
  L.over(soften((np.abs(x) < side / 2) & (np.abs(y) < side / 2), 0.3))
  return L


# Square tiles and mandalas alternate on a 20 mm grid. Each grid corner
# meets two tiles and two mandalas; a scroll ornament lies there along the
# line between the mandalas, where the gap is widest. A small square sits
# at the middle of each grid edge. The half-drop repeat (20, 20) maps tile
# to tile, so the tile is 20 x 40 mm with one square tile and one mandala.

W1_TILE = Tile(20.0, 40.0, origin=(16.0, 36.0))


def w1_placements():
  return [(g_small_square, 10.0, 0.0, 0, 2.5), (g_small_square, 0.0, 10.0, 0, 2.5),
          (g_small_square, 10.0, 20.0, 0, 2.5), (g_small_square, 0.0, 30.0, 0, 2.5),
          (g_scroll, 10.0, 10.0, -45, 6.0), (g_scroll, 10.0, 30.0, 45, 6.0),
          (g_square, 0.0, 0.0, 0, 9.5), (g_mandala, 0.0, 20.0, 0, 7.5)]


def w1_tiles():
  return draw(W1_TILE, w1_placements())


# --- W2: paisley with flowers -----------------------------------------------------

# Discs swept along the spine, y up, from the body centre up the back and
# over into the curl, shrinking from the body radius to the tip radius.

PAISLEY_SPINE = [(0.0, 0.0), (1.2, 7.0), (-1.0, 12.5), (-4.2, 9.6)]
PAISLEY_R = (5.6, 0.45, 1.5)               # body radius, tip radius, taper power


def ccw_arc(c, r, p0, p1, step=0.05):
  """Points on the circle (c, r) counter-clockwise from p0 to p1, both excluded."""
  a0 = math.atan2(p0[1] - c[1], p0[0] - c[0])
  a1 = math.atan2(p1[1] - c[1], p1[0] - c[0])
  a1 += 2 * math.pi if a1 <= a0 else 0
  t = np.linspace(a0, a1, max(3, int((a1 - a0) * r / step)))[1:-1]
  return np.stack([c[0] + r * np.cos(t), c[1] + r * np.sin(t)], 1)


@lru_cache
def paisley_outline(length):
  """Closed counter-clockwise outline of the swept discs, points about
  0.12 mm apart, scaled to `length` mm tall and centred on its bounding box,
  and where the body centre lands."""
  pts, s = bezier(PAISLEY_SPINE, step=0.02)
  r0, r1, power = PAISLEY_R
  w = r1 + (r0 - r1) * (1 - s / s[-1]) ** power
  t = np.gradient(pts, s, axis=0)
  t /= np.hypot(t[:, 0], t[:, 1])[:, None]
  nrm = np.stack([-t[:, 1], t[:, 0]], 1)
  fwd = -np.gradient(w, s)[:, None]
  side = np.sqrt(1 - fwd * fwd)
  left = pts + w[:, None] * (fwd * t + side * nrm)
  right = pts + w[:, None] * (fwd * t - side * nrm)
  ring = np.concatenate([right, ccw_arc(pts[-1], w[-1], right[-1], left[-1]),
                         left[::-1], ccw_arc(pts[0], w[0], left[0], right[0])])
  arc = np.concatenate([[0.0], np.cumsum(np.hypot(*np.diff(ring, axis=0).T))])
  even = np.arange(0, arc[-1], 0.12)
  ring = np.stack([np.interp(even, arc, ring[:, 0]), np.interp(even, arc, ring[:, 1])], 1)
  mid = (ring.min(0) + ring.max(0)) / 2
  k = length / (ring[:, 1].max() - ring[:, 1].min())
  return (ring - mid) * k, (pts[0] - mid) * k


def contour_marks(pts, dm, spacing, need):
  """Points about `spacing` mm apart along the curve `dm` mm inside the
  closed counter-clockwise outline pts, each with its inward normal; only
  where the outline stays `need` mm deep along that normal."""
  t = np.roll(pts, -1, 0) - np.roll(pts, 1, 0)
  t /= np.hypot(t[:, 0], t[:, 1])[:, None]
  n = np.stack([-t[:, 1], t[:, 0]], 1)
  q, deep = pts + dm * n, pts + need * n

  def depth(p):
    return -polygon_sdf(pts, p[:, 0], p[:, 1])

  ok = (np.abs(depth(q) - dm) < 0.03) & (np.abs(depth(deep) - need) < 0.03)
  start = int(np.argmin(ok))
  q, n, ok = (np.roll(a, -start, 0) for a in (q, n, ok))
  marks = []
  i = 0
  while i < len(ok):
    if not ok[i]:
      i += 1
      continue
    j = i
    while j < len(ok) and ok[j]:
      j += 1
    run = q[i:j]
    s = np.concatenate([[0.0], np.cumsum(np.hypot(*np.diff(run, axis=0).T))])
    count = int(s[-1] // spacing)
    for k in range(count):
      m = int(np.searchsorted(s, (k + 0.5) * s[-1] / count))
      marks.append((run[m], n[i + m]))
    i = j
  kept = []
  for p, nn in marks:
    if all(np.hypot(*(p - k)) > 0.9 * spacing for k, _ in kept):
      kept.append((p, nn))
  return kept


def g_paisley(x, y, length, inside, flip=1):
  """Paisley `length` mm tall, curl at the top: a 0.9 mm light outline band,
  a dark gap, then `inside`. 'petals': a ring of round-ended petals where
  the paisley is wide enough, a row of dots carrying it up the tail, round
  a light teardrop seed with a dark eye. 'dots': a row of dots round a light
  centre. flip=-1 mirrors it. The dark gap ends in a round cap where the
  tail narrows."""
  L = Layers(x.shape)
  pts, (cx, cy) = paisley_outline(length)
  bx, by = flip * x, -y
  depth = -polygon_sdf(pts, bx, by)
  body = depth >= 0
  light = body & ~opening(depth >= 0.9, disc(13))
  dots = contour_marks(pts, 2.03, 1.65, 2.85)
  if inside == 'petals':
    petals = contour_marks(pts, 2.45, 2.0, 3.65)
    for q, n in petals:
      X = (bx - q[0]) * n[0] + (by - q[1]) * n[1] + 0.9
      Y = -(bx - q[0]) * n[1] + (by - q[1]) * n[0]
      light |= spoon(X, Y, 0.0, 1.8, 0.6, 0.42) < 0
    dots = [(q, n) for q, n in dots if min(np.hypot(*(q - p)) for p, _ in petals) > 1.9]
    light |= opening(depth >= 4.0, disc(9)) & (np.hypot(bx - cx, by - cy) >= 0.45)
  else:
    light |= opening(depth >= 3.15, disc(9))
  for q, _ in dots:
    light |= np.hypot(bx - q[0], by - q[1]) < 0.475
  L.over(body, light & body)
  return L


def g_flower6s(x, y):
  """Six round-tipped petals, 8 mm."""
  L = Layers(x.shape)
  spoon_flower(L, x, y, 6, 4.0, 0.8, 0.9, base_r=0.45)
  return L


def g_blossom(x, y):
  """Five round petals parted by dark divides, 7 mm."""
  L = Layers(x.shape)
  blossom5(L, x, y, 3.5)
  return L


def g_flower8s(x, y):
  """Eight round-tipped petals, 7 mm."""
  L = Layers(x.shape)
  spoon_flower(L, x, y, 8, 3.5, 0.75, 0.66, base_r=0.4)
  return L


BIG = partial(g_paisley, length=18.0, inside='petals')
SMALL = partial(g_paisley, length=13.0, inside='dots')

# Hand-placed in a 28 x 30 mm half-drop tile: tile mm u, v, turn, patch half-size.

W2_TILE = Tile(28.0, 30.0, origin=(1.0, 2.0))


def w2_placements():
  return [(partial(dot, d=1.3), 14.9, 1.9, 0, 1.5),
          (partial(dot, d=1.3), 14.0, 20.6, 0, 1.5),
          (partial(dot, d=1.3), 26.6, 12.2, 0, 1.5),
          (partial(dot, d=1.3), 15.2, 13.8, 0, 1.5),
          (partial(dot, d=1.3), 10.2, 21.6, 0, 1.5),
          (g_flower6s, 20.5, 6.6, 10, 4.5),
          (g_blossom, 4.7, 25.3, 0, 4.0),
          (g_flower8s, 13.6, 27.4, 0, 4.0),
          (BIG, 7.5, 10.3, 20, 10.5),
          (partial(SMALL, flip=-1), 22.4, 21.6, 200, 8.0)]


def w2_paisley():
  return draw(W2_TILE, w2_placements())


# --- Output -------------------------------------------------------------------

CANDIDATES = [('W0', w0_folk, W0_TILE, w0_placements),
              ('W1', w1_tiles, W1_TILE, w1_placements),
              ('W2', w2_paisley, W2_TILE, w2_placements)]


def main(args):
  if '--map' in args:
    for name, fn, tile, places in CANDIDATES:
      if name in args:
        tile_map(tile, places())
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
