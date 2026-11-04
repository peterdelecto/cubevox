#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family J: separate light squares only.

Each candidate redraws one circle pattern (I0 to I4 from candidates_i.py, C12
from candidates_c.py) as axis-aligned squares, one per circle, centred on it.
A square's half-side is the circle's radius, cut down so every pair keeps a
0.7 mm dark gap in the Chebyshev metric. Squares are stamped pixel-exact and
checked by a square-element print pass, so their corners stay sharp.

  candidates_j.py           every candidate's face PNG; prints the report lines
  candidates_j.py J0 J5     only the named candidates
"""

import subprocess
import sys
import time

import numpy as np
from scipy.ndimage import binary_closing, binary_opening
from scipy.spatial import cKDTree

from candidates_b import ALLOWED, H, OUT, S, W, face_image
from candidates_c import c12_circle_list
from candidates_i import i0_list, i1_list, i2_list, i3_list, i4_list

GAP = 0.7          # dark gap between squares, mm; keeps the rasterised gap at 7 px or more
EDGE = 0.15        # margin inside the field, mm
HALF_MIN = 0.4     # smallest half-side kept, mm (0.8 mm side)
CELL = 4.0         # neighbour grid, mm; wider than any two half-sides plus the gap


def in_allowed(u, v):
  j, i = int(v * S), int(u * S)
  return 0 <= j < H and 0 <= i < W and ALLOWED[j, i]


def squares_from_circles(circles):
  """Greedy squares (u, v, half) in the circles' order. Each takes its
  circle's radius as half-side, shrunk to keep GAP to every square already
  placed; it is dropped under HALF_MIN or when its centre or a corner of the
  square grown by EDGE leaves ALLOWED. Returns (squares, dropped)."""
  grid, squares, dropped = {}, [], 0
  for u, v, r in circles:
    half = r
    ci, cj = int(u // CELL), int(v // CELL)
    for a in (ci - 1, ci, ci + 1):
      for b in (cj - 1, cj, cj + 1):
        for pu, pv, ph in grid.get((a, b), ()):
          half = min(half, max(abs(pu - u), abs(pv - v)) - ph - GAP)
    e = half + EDGE
    if half < HALF_MIN or not all(in_allowed(u + du, v + dv)
                                  for du, dv in ((0, 0), (-e, -e), (e, -e), (-e, e), (e, e))):
      dropped += 1
      continue
    sq = (u, v, half)
    squares.append(sq)
    grid.setdefault((ci, cj), []).append(sq)
  return squares, dropped


def draw_squares(squares):
  light = np.zeros((H, W), bool)
  for u, v, half in squares:
    side = max(8, round(2 * half * S))
    i0, j0 = round((u - half) * S), round((v - half) * S)
    light[j0:j0 + side, i0:i0 + side] = True
  return light


def printable_square(light):
  """Opening by an 8 x 8 square, then closing by a 6 x 6 square, inside
  ALLOWED. Returns the mask and the fraction of light pixels that changed."""
  start = light & ALLOWED
  m = binary_closing(binary_opening(start, np.ones((8, 8), bool)), np.ones((6, 6), bool)) & ALLOWED
  return m, np.count_nonzero(m != start) / np.count_nonzero(start)


def min_gap(squares):
  """Smallest Chebyshev gap between any two squares, mm."""
  q = np.array(squares)
  reach = 2 * q[:, 2].max() + GAP + 0.01
  pairs = cKDTree(q[:, :2]).query_pairs(reach, p=np.inf, output_type='ndarray')
  d = np.abs(q[pairs[:, 0], :2] - q[pairs[:, 1], :2]).max(axis=1)
  return (d - q[pairs[:, 0], 2] - q[pairs[:, 1], 2]).min()


CANDIDATES = [
  ('J0', 'I0', i0_list, 'I0 frost redrawn as squares, one per stuck lattice cell'),
  ('J1', 'I1', i1_list, 'I1 coral stripes redrawn as beaded worms of squares'),
  ('J2', 'I2', i2_list, 'I2 sunflower whorls redrawn as squares'),
  ('J3', 'I3', i3_list, 'I3 foam packing redrawn as squares'),
  ('J4', 'I4', i4_list, 'I4 body folds redrawn as squares'),
  ('J5', 'C12', c12_circle_list, 'C12 tissue circles redrawn as squares'),
]


def main(names):
  OUT.mkdir(parents=True, exist_ok=True)
  field = np.count_nonzero(ALLOWED)
  for name, source, circles_fn, what in [c for c in CANDIDATES if not names or c[0] in names]:
    t0 = time.time()
    squares, dropped = squares_from_circles(circles_fn())
    light, changed = printable_square(draw_squares(squares))
    path = OUT / f'{name}.png'
    face_image(light).save(path)
    md5 = subprocess.run(['md5', '-q', str(path)], capture_output=True, text=True,
                         check=True).stdout.strip()
    gap = min_gap(squares)
    print(f'{name} (from {source}): {len(squares)} squares kept, {dropped} dropped, '
          f'coverage {100 * np.count_nonzero(light) / field:.1f} %, min gap {gap:.3f} mm, '
          f'changed {changed * 100:.2f} %, md5 {md5}, {time.time() - t0:.1f} s', flush=True)
    if gap < GAP - 1e-9:
      print(f'  {name} gap rule: min gap {gap:.3f} mm', flush=True)
    if changed > 0.005:
      print(f'  {name} print pass: changed {changed * 100:.2f} %', flush=True)
    print(f'  {name}: {what}.', flush=True)


if __name__ == '__main__':
  main(sys.argv[1:])
