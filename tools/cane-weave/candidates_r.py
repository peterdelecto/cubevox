#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family R: a pharmacy scatter.

Syringes, ampules, vials and capsules drawn as flat light icons with dark
detail lines cut through them. Each motif is a signed distance field in its
own frame: a runs along the axis from the tip end, b across, mm, negative
inside the light. Joins are smooth unions, so concave corners carry fillets,
and detail lines are smooth cuts, so the light corners they leave are round.
Every dark line is 0.8 mm; every light rim is 1.0 mm or more.

One square tile, repeated in half-drop: the column to the right of a tile
sits half a tile lower. Same face grid and report as candidates_b.

  candidates_r.py           every candidate as <name>.png
  candidates_r.py R0        only the named candidates
"""

import math
import sys

import numpy as np

import candidates_b as cb
from candidates_b import ALLOWED, S, U, V

LINE = 0.8      # dark detail line width, mm
CUT_K = 0.3     # corner radius scale where a line cuts the light


# --- Distance-field tools ---------------------------------------------------

def box(a, b, ca, cb_, ha, hb, r=0.0):
  """Box centred at (ca, cb_) with half extents ha, hb and corner radius r."""
  qa = np.abs(a - ca) - (ha - r)
  qb = np.abs(b - cb_) - (hb - r)
  return (np.hypot(np.maximum(qa, 0), np.maximum(qb, 0))
          + np.minimum(np.maximum(qa, qb), 0) - r)


def seg(a, b, a0, b0, a1, b1, w):
  """Stroke of width w with round caps from (a0, b0) to (a1, b1)."""
  pa, pb, da, db = a - a0, b - b0, a1 - a0, b1 - b0
  t = np.clip((pa * da + pb * db) / (da * da + db * db), 0, 1)
  return np.hypot(pa - t * da, pb - t * db) - w / 2


def trap(a, b, a0, a1, h0, h1, r=0.0):
  """Symmetric trapezoid along a from a0 (half width h0) to a1 (half width
  h1), corners rounded by r."""
  he, r1, r2 = (a1 - a0) / 2 - r, h0 - r, h1 - r
  y, x = a - (a0 + a1) / 2, np.abs(b)
  k1a, k1b, k2a, k2b = r2, he, r2 - r1, 2 * he
  ca_x = x - np.minimum(x, np.where(y < 0, r1, r2))
  ca_y = np.abs(y) - he
  t = np.clip(((k1a - x) * k2a + (k1b - y) * k2b) / (k2a * k2a + k2b * k2b), 0, 1)
  cb_x, cb_y = x - k1a + k2a * t, y - k1b + k2b * t
  s = np.where((cb_x < 0) & (ca_y < 0), -1.0, 1.0)
  return s * np.sqrt(np.minimum(ca_x ** 2 + ca_y ** 2, cb_x ** 2 + cb_y ** 2)) - r


def smin(d0, d1, k):
  """Union with a fillet about k/4 deep in the concave corners."""
  h = np.maximum(k - np.abs(d0 - d1), 0) / k
  return np.minimum(d0, d1) - h * h * k / 4


def cut(light, dark, k=CUT_K):
  """Light less dark, the corners left in the light rounded."""
  return -smin(-light, dark, k)


# --- Motifs -----------------------------------------------------------------

def syringe(a, b):
  """26.5 mm: needle 1.0 mm, hub, nozzle, 4 mm barrel with five ticks,
  finger flange, plunger rod, thumb pad."""
  barrel = box(a, b, 0.5, 0, 5.5, 2.0, 0.6)
  nozzle = box(a, b, -5.3, 0, 0.7, 0.7, 0.2)
  hub = box(a, b, -6.6, 0, 0.8, 1.2, 0.4)
  needle = seg(a, b, -13.0, 0, -7.0, 0, 1.0)
  flange = box(a, b, 6.6, 0, 0.6, 4.0, 0.6)
  rod = box(a, b, 9.4, 0, 2.6, 0.8, 0.2)
  pad = box(a, b, 12.4, 0, 0.6, 2.8, 0.6)
  light = smin(smin(smin(needle, hub, 0.4), nozzle, 0.3), barrel, 0.3)
  light = smin(smin(smin(light, flange, 0.5), rod, 0.4), pad, 0.4)

  # Ticks hang from one wall, alternately long and short, 1.0 mm rims.

  ticks = np.full(a.shape, np.inf)
  for i, at in enumerate((-3.6, -1.7, 0.2, 2.1, 4.0)):
    end = 1.0 if i % 2 == 0 else 0.2
    ticks = np.minimum(ticks, box(a, b, at, (end - 1.0) / 2, LINE / 2, (end + 1.0) / 2, 0.3))
  return cut(light, ticks)


def ampule(a, b):
  """15 mm: round-bottomed 5 mm body, shoulder into a 1.8 mm neck, dark
  break ring, tip flaring to 3.4 mm."""
  body = seg(a, b, 1.8, 0, 5.0, 0, 5.0)
  neck = box(a, b, -2.4, 0, 2.4, 0.9)
  head = trap(a, b, -7.5, -4.4, 1.7, 0.9, 0.5)
  light = smin(smin(body, neck, 2.4), head, 0.4)
  return cut(light, np.abs(a + 2.6) - LINE / 2)


def vial(a, b):
  """15 mm: 6.8 mm body, tapered shoulder, 3.2 mm neck, 5 mm crimp cap
  with a dark seam. The label's two dark lines stop 1.0 mm short of the
  walls, so the bottle outline stays whole."""
  body = box(a, b, 3.15, 0, 4.35, 3.4, 1.0)
  shoulder = trap(a, b, -2.6, -0.2, 1.6, 3.4, 0.4)
  neck = box(a, b, -2.7, 0, 0.9, 1.6)
  cap = box(a, b, -5.45, 0, 2.05, 2.5, 0.6)
  light = smin(smin(smin(body, shoulder, 0.6), neck, 0.5), cap, 0.3)
  label = np.minimum(box(a, b, 1.6, 0, LINE / 2, 2.4, 0.3), box(a, b, 5.4, 0, LINE / 2, 2.4, 0.3))
  return cut(light, np.minimum(label, np.abs(a + 5.6) - LINE / 2))


def capsule(a, b, two_tone=False):
  """11 x 4.8 mm pill with one dark divide; two-tone draws the half at
  a > 0 as a 1.0 mm light outline."""
  light = seg(a, b, -3.1, 0, 3.1, 0, 4.8)
  dark = np.abs(a) - LINE / 2
  if two_tone:
    inner = -smin(-(light + 1.0), -(LINE / 2 + 1.0 - a), 0.8)
    dark = np.minimum(dark, inner)
  return cut(light, dark)


MOTIFS = {
  'syringe': (syringe, 13.6),
  'ampule': (ampule, 7.6),
  'vial': (vial, 7.9),
  'capsule': (capsule, 5.6),
  'capsule2': (lambda a, b: capsule(a, b, True), 5.6),
}


# --- R0: the tile -----------------------------------------------------------

TILE = 68.0                 # mm square; the column to the right drops TILE / 2
ORIGIN = (4.0, 9.0)         # face (u, v) of the tile's top-left corner, mm

# (motif, u, v, angle) in the tile, mm and degrees; angle turns the motif's
# axis from +u toward +v (clockwise on the page).

PLACEMENTS = [
  ('syringe', 67.4, 32.8, 20),
  ('vial', 49.2, 62.4, 158),
  ('ampule', 23.2, 36.4, 295),
  ('capsule', 4.0, 25.6, 73),
  ('syringe', 5.6, 49.2, 210),
  ('capsule2', 42.2, 44.6, 348),
  ('vial', 24.6, 12.6, 125),
  ('ampule', 40.6, 20.2, 263),
  ('capsule', 23.4, 52.0, 40),
  ('vial', 62.0, 47.6, 178),
  ('capsule2', 55.0, 8.4, 315),
  ('ampule', 15.4, 28.6, 93),
  ('capsule', 60.6, 20.2, 230),
  ('vial', 18.4, 62.8, 8),
  ('ampule', 32.6, 40.0, 145),
  ('capsule2', 62.8, 63.6, 283),
  ('capsule', 8.4, 39.6, 60),
  ('vial', 51.2, 35.6, 198),
  ('ampule', 43.8, 54.2, 335),
  ('capsule', 30.6, 21.4, 113),
  ('capsule2', 8.4, 14.0, 250),
  ('ampule', 42.6, 5.4, 165),
  ('capsule', 17.2, 6.8, 303),
  ('capsule2', 49.4, 21.0, 80),
  ('capsule', 33.0, 66.4, 133),
]


def r0_pharmacy():
  light = np.zeros(U.shape, bool)
  for kind, pu, pv, ang in PLACEMENTS:
    fn, reach = MOTIFS[kind]
    c, s = math.cos(math.radians(ang)), math.sin(math.radians(ang))
    for n1 in range(-2, 5):
      for n2 in range(-3, 5):
        cu = ORIGIN[0] + pu + n1 * TILE
        cv = ORIGIN[1] + pv + n1 * TILE / 2 + n2 * TILE
        j0, j1 = max(int((cu - reach) * S), 0), min(int((cu + reach) * S) + 1, U.shape[1])
        i0, i1 = max(int((cv - reach) * S), 0), min(int((cv + reach) * S) + 1, U.shape[0])
        if j0 >= j1 or i0 >= i1:
          continue
        du, dv = U[i0:i1, j0:j1] - cu, V[i0:i1, j0:j1] - cv
        light[i0:i1, j0:j1] |= fn(du * c + dv * s, -du * s + dv * c) < 0
  return light


CANDIDATES = [
  ('R0', r0_pharmacy,
   'pharmacy scatter: syringes, ampules, vials and capsules, flat light icons with 0.8 mm dark '
   'detail lines, one 68 mm tile in half-drop'),
]


def main(names):
  cb.OUT.mkdir(parents=True, exist_ok=True)
  for name, fn, what in CANDIDATES:
    if not names or name in names:
      img, _ = cb.render(name, fn)
      px = np.asarray(img)
      lit = np.all(px == cb.LIGHT, axis=2)
      print(f'  light covers {100 * np.count_nonzero(lit) / np.count_nonzero(ALLOWED):.1f} % of the field')


if __name__ == '__main__':
  main(sys.argv[1:])
