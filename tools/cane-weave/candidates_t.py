#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family T: a banded textile print.

One grid of 1.6 mm square cells covers the face, 120 across and 95 down,
cell (0, 0) at the face's top-left corner, so each cell is 16 x 16 px. Each
cell is light or dark, and every shape is stepped on the grid like weaving.
Bands run across the face, each a tile repeated along it, and stack down it.
The window and the holes cut the cloth wherever they fall. The motifs come
from one banded print:

  (a) bow-ties alternating with dots
  (b) a dark stepped zigzag line through a light band
  (c) straight stripes
  (d) dotted chevrons with an arch at the foot, small triangles stepping up
      their outer edges and a tassel hanging between each pair
  (e) stepped diamonds nested in rings and ringed by triangle teeth

No two light cells, and no two dark cells, meet only at a corner, so the
print pass trims nothing but square corners. The face grid, the
printability pass and the output come from candidates_b.py.

  candidates_t.py           every candidate's face PNG; prints the report lines
  candidates_t.py T0 T3     only the named candidates
"""

import hashlib
import sys

import numpy as np

from candidates_b import ALLOWED, H, LIGHT, OUT, S, W, render

CELL = 1.6                     # mm
PX = round(CELL * S)
ROWS, COLS = H // PX, W // PX
assert ROWS * PX == H and COLS * PX == W


# --- Laying bands -------------------------------------------------------------

def across(tile, x0=0):
  """A tile repeated across the face, its column 0 on face column x0."""
  return tile[:, (np.arange(COLS) - x0) % tile.shape[1]]


def dark(n):
  return np.zeros((n, COLS), bool)


def down(strip, r0):
  """A strip of bands repeated down the face, its row 0 on face row r0."""
  return strip[(np.arange(ROWS) - r0) % strip.shape[0]]


def corner_joins(cells):
  """2 x 2 blocks where two cells of one colour meet only at a corner."""
  a, b, c, d = cells[:-1, :-1], cells[:-1, 1:], cells[1:, :-1], cells[1:, 1:]
  return int(np.count_nonzero((a == d) & (b == c) & (a != b)))


# --- The bands ----------------------------------------------------------------

def band_a(h=7):
  """Bow-ties on the tile edges and a dot between them, repeat 15 cells
  (24 mm). The dots land on the hole columns."""
  r, c = np.mgrid[0:h, 0:15]
  x = c + 0.5
  ry = np.abs(r - h // 2)
  bx = np.abs((x + 7.5) % 15 - 7.5)
  bow = (bx > ry) & (bx < h // 2 + 1)
  dx = np.abs(x - 7.5)
  rad = (h - 3) // 2
  dot = (ry <= rad) & (dx + ry <= rad + 1) & (dx <= 2)
  return bow | dot


def band_b(amp, flip=False):
  """A light band crossed by a dark stepped zigzag line two rows thick,
  teeth 2 * amp cells long and amp rows high."""
  r, c = np.mgrid[0:amp + 4, 0:2 * amp]
  y = 1 + np.abs(c - amp)
  light = (r != y) & (r != y + 1)
  return light[::-1] if flip else light


def stripes(*rows):
  """Straight stripes, one value per row, 1 light and 0 dark."""
  return np.array(rows, bool)[:, None]


# Chevron arms are 6 cells thick so each 2 x 2 dot keeps 2 cells of arm on
# either side; a thinner border would meet the arm's steps at a corner.

TASSEL = {0: 4, 1: 3, 2: 2, 3: 1, 5: 1, 6: 1, 7: 1, 8: 1, 9: 1, 10: 1, 12: 1, 13: 1,
          15: 3, 16: 2, 18: 2, 19: 1, 21: 1}
ARCH = {17: (0, 1), 18: (0, 1), 19: (1, 2), 20: (1, 2), 21: (2, 3)}


def band_d():
  """Chevrons 22 cells wide and 22 tall pointing up, repeat 30 cells (48 mm).
  Each half-width grows one cell every two rows. A light arch stands in the
  dark inside at the foot; three small triangles step up each outer edge;
  a tassel hangs on each tile edge: a triangle, a stem, a bead and a stack
  of three shrinking slices."""
  r, c = np.mgrid[0:22, 0:30]
  i = np.abs(c - 14.5) - 0.5                 # cells out from the chevron centre
  t = np.abs((c + 15) % 30 - 14.5) - 0.5     # cells out from the tassel centre
  ho = 1 + r // 2
  light = (i < ho) & (i >= ho - 6)

  dots = (r >= 4) & (r <= 5) & (i == 0)
  for r0 in (8, 12, 16):
    h0 = 1 + r0 // 2
    dots |= (r >= r0) & (r <= r0 + 1) & ((i == h0 - 4) | (i == h0 - 3))
  light &= ~dots

  for rr, cells in ARCH.items():
    light |= (r == rr) & np.isin(i, cells)
  for r0 in (1, 7, 13):
    s = 2 + (r0 + 1) // 2
    light |= ((r == r0) & (i >= s) & (i <= s + 2)) | ((r == r0 + 1) & (i == s + 1))
  for rr, half in TASSEL.items():
    light |= (r == rr) & (t < half)
  return light


def band_e():
  """A stepped diamond 19 cells across, repeat 20 cells (32 mm): a dark
  centre, a light ring, a dark ring, a light ring out to the points, a dark
  moat, and eight triangle teeth, two flanking each point."""
  r, c = np.mgrid[0:19, 0:20]
  u, w = np.abs(c - 9), np.abs(r - 9)
  d = u + w
  light = ((d >= 2) & (d <= 4)) | ((d >= 7) & (d <= 9))
  for a, b in ((u, w), (w, u)):
    light |= (a >= 3) & (a <= 5) & (b >= 7) & (b <= 9) & (a + b >= 12)
  return light


def zigzag_line():
  """A light stepped zigzag line two cells thick, teeth 4 cells long."""
  r, c = np.mgrid[0:4, 0:4]
  y = np.abs(c - 2)
  return (r == y) | (r == y + 1)


# --- Candidates ---------------------------------------------------------------

def t0():
  """The whole stack, 75 rows (120 mm): a, b, c, d, c, b, e. The chevrons
  sit below the window."""
  strip = np.vstack([
      across(band_a()), dark(1), across(band_b(3)), across(stripes(0, 1, 1, 0)),
      across(band_d()), across(stripes(0, 1, 1, 1, 0)), across(band_b(3, flip=True)),
      dark(1), across(band_e(), 10), dark(2)])
  return down(strip, 22)


def t1():
  sep = np.vstack([dark(1), across(zigzag_line()), dark(1)])
  strip = np.vstack([across(band_e(), 0), sep, across(band_e(), 10), sep])
  return down(strip, 38)


def t2():
  d = band_d()
  strip = np.vstack([across(d), dark(1), across(d[::-1]), dark(1)])
  return down(strip, 2)


def t3():
  c = across(stripes(0, 1, 0, 1, 0))
  strip = np.vstack([across(band_a()), c, across(band_b(2)), c])
  return down(strip, 2)


def t4():
  """Diamonds 40 cells (64 mm) wide and tall, centred on cell corners: a
  light centre and six rings 3 cells thick, dark and light by turns, echoing
  on into the gaps between diamonds. The two rows of diamonds sit half a
  diamond apart across, with a 5-row band (a) between them; each row's
  rings run on to the face edge above or below."""
  R, C = np.mgrid[0:ROWS, 0:COLS]
  upper = R < 47
  cy = np.where(upper, 24, 71)
  shift = np.where(upper, 20, 0)
  dx = np.abs((C + 0.5 - shift) % 40 - 20)
  dy = np.abs(R + 0.5 - cy)
  d = (dx + dy).astype(int)
  light = (d <= 2) | (((d - 3) // 3) % 2 == 1)
  light[44:51] = False
  light[45:50] = across(band_a(5))
  return light


CANDIDATES = [
  ('T0', t0, 'the reference stack', 'stack 75 rows (120 mm): a 7 rows, b 7 rows, '
   'c 4, d 22, c 5, b 7, e 19; a repeats 24 mm, b teeth 9.6 mm (48 mm repeat), '
   'd 48 mm, e 32 mm'),
  ('T1', t1, 'stepped diamonds', 'diamonds 19 cells (30.4 mm) in a 32 mm repeat, '
   'row pitch 25 rows (40 mm), zigzag line teeth 6.4 mm, rows offset 16 mm'),
  ('T2', t2, 'chevrons', 'chevrons 35.2 mm wide in a 48 mm repeat, 22 rows (35.2 mm) '
   'tall, row pitch 23 rows (36.8 mm), every second row flipped'),
  ('T3', t3, 'fine bands', 'a 7 rows (11.2 mm, 24 mm repeat), c 5 rows (8 mm), '
   'b 6 rows (9.6 mm, teeth 6.4 mm, 32 mm repeat), sequence a-c-b-c every 36.8 mm'),
  ('T4', t4, 'eye-dazzler', 'diamonds 64 mm in a 64 mm repeat, rows offset 32 mm, '
   'six rings 3 cells thick, band a 5 rows (8 mm) between the two rows, row pitch 75.2 mm'),
]


def main(names):
  OUT.mkdir(parents=True, exist_ok=True)
  field = np.count_nonzero(ALLOWED)
  for name, fn, what, sizes in [c for c in CANDIDATES if not names or c[0] in names]:
    cells = fn()
    joins = corner_joins(cells)
    img, changed = render(name, lambda: cells.repeat(PX, 0).repeat(PX, 1))
    lit = np.count_nonzero(np.all(np.asarray(img) == LIGHT, axis=2))
    md5 = hashlib.md5((OUT / f'{name}.png').read_bytes()).hexdigest()
    print(f'{name} ({what}): {sizes}; coverage {100 * lit / field:.1f} %, '
          f'changed {changed * 100:.2f} %, corner joins {joins}, md5 {md5}', flush=True)


if __name__ == '__main__':
  main(sys.argv[1:])
