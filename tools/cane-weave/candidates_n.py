#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family N: clean interlace.

Every strand is a light band of one width drawn from a distance field round
its centre line. Where a strand passes under another it loses every pixel
within half the top strand's width plus 0.8 mm of the top strand's centre
line, near the crossing, so a dark seam shows the dive. Nothing is shaded.
Convex corners are rounded to 0.5 mm and dark specks under 0.7 mm filled.
Same face grid and report as candidates_b.

  candidates_n.py           every candidate as <name>.png
  candidates_n.py N0 N3     only the named candidates
"""

import math
import sys

import numpy as np

import candidates_b as cb
from candidates_b import ALLOWED, COLUMN_U, U, V, closing, disc, opening

SEAM = 0.8
FILLET = disc(10)
SPECK = disc(7)


def finish(light):
  """Mask with ALLOWED, round convex corners, fill dark specks; print the
  share of light pixels this changed."""
  start = light & ALLOWED
  done = closing(opening(start, FILLET), SPECK) & ALLOWED
  print(f'  corner rounding changed {100 * np.count_nonzero(done != start) / np.count_nonzero(start):.2f} %')
  return done


def lines(angle, pitch, offset=0.0):
  """Nearest of the lines whose normal points at `angle` degrees (page
  coordinates, v down), at offset + k * pitch along that normal: the index k
  and the signed distance, mm."""
  a = math.radians(angle)
  s = U * math.cos(a) + V * math.sin(a) - offset
  k = np.round(s / pitch)
  return k, s - k * pitch


def to_crossing(da, db, phi):
  """Distance to the crossing of two lines, from the signed distances to each
  and the angle between their normals in degrees."""
  c, s = math.cos(math.radians(phi)), abs(math.sin(math.radians(phi)))
  return np.sqrt(np.maximum(da * da + db * db - 2 * c * da * db, 0)) / s


def straight_weave(fams, width, over, seam=SEAM):
  """Families of straight bands, each (normal angle, pitch, offset).
  over(b, kb, a, ka) is True where band kb of family b lies over band ka of
  family a at their crossing."""
  idx = [lines(*f) for f in fams]
  half = width / 2
  light = np.zeros(U.shape, bool)
  for a, (ka, da) in enumerate(idx):
    keep = np.abs(da) < half
    for b, (kb, db) in enumerate(idx):
      if b != a:
        near = (np.abs(db) < half + seam) & (to_crossing(da, db, fams[b][0] - fams[a][0]) < 1.5 * width)
        keep &= ~(near & over(b, kb, a, ka))
    light |= keep
  return finish(light)


def plain(b, kb, a, ka):
  """Plain weave: family 0 on top where the band indices sum even."""
  return ((kb + ka) % 2 == 0) == (b == 0)


# --- N0, N1: plain weave ----------------------------------------------------

def n0_basket():
  return straight_weave([(90, 6.0), (0, 6.0)], 4.0, plain)


def n1_lattice():
  p = 6.0 * math.sqrt(2)
  return straight_weave([(45, p), (135, p)], 4.0, plain)


# --- N2: crochet chain rows -------------------------------------------------

def stadium(x, y, half_straight, radius):
  """Distance to a stadium centre line: a straight run of 2 * half_straight
  along x, capped by semicircles of `radius`."""
  return np.abs(np.hypot(np.maximum(np.abs(x) - half_straight, 0), y) - radius)


def n2_chain():
  """Rows of stadium links 6 mm apart along the row, alternate rows shifted
  3 mm. Each link lies over the next at the bottom crossing and under it at
  the top one, so every pair links and over and under alternate round each
  link. The 1.2 mm band is the widest that keeps 0.8 mm seams inside each
  link and between rows at this size; the ends cross at 60 degrees. At a
  4 mm step each link would also cross the link two ahead."""
  w, a, b, step, rh = 1.2, 4.0, 2.0, 6.0, 6.0
  s = a - b
  cy = math.sqrt(b * b - (step / 2 - s) ** 2)
  row = np.round(V / rh)
  y = V - rh * row
  off = COLUMN_U + (row % 2) * step / 2
  light = np.zeros(U.shape, bool)
  for q in range(2):
    n = 2 * np.round(((U - off) / step - q) / 2) + q
    x = U - off - n * step
    keep = stadium(x, y, s, b) < w / 2

    # Under the next link at the top crossing, under the previous at the bottom.

    for sx, sy in ((1, -1), (-1, 1)):
      keep &= ~((stadium(x - sx * step, y, s, b) < w / 2 + SEAM)
                & (np.hypot(x - sx * step / 2, y - sy * cy) < 1.5 * w))
    light |= keep
  return finish(light)


# --- N3: knit ---------------------------------------------------------------

def n3_knit():
  """U loops, rows 6 mm apart, alternate rows shifted half a stitch. The
  head of each U lies over the legs of the row below; each leg rises and
  ends under the head of the loop above."""
  w, a, cw, rh = 2.0, 2.6, 8.0, 6.0
  cross_v = -rh + math.sqrt(a * a - (cw / 2 - a) ** 2)
  lg = -cross_v
  light = np.zeros(U.shape, bool)
  for p in range(2):
    row = 2 * np.round(((V + 0.6) / rh - p) / 2) + p
    y = V - rh * row
    off = COLUMN_U + (row % 2) * cw / 2
    x = U - off - np.round((U - off) / cw) * cw
    ax = np.abs(x)
    d = np.where(y >= 0, np.abs(np.hypot(x, y) - a),
                 np.where(y >= -lg, np.abs(ax - a), np.hypot(ax - a, y + lg)))
    keep = d < w / 2
    sx = np.where(x < 0, -1.0, 1.0)
    keep &= ~((y < 0) & (np.abs(np.hypot(x - sx * cw / 2, y + rh) - a) < w / 2 + SEAM)
              & (np.hypot(x - sx * a, y - cross_v) < 1.5 * w))
    light |= keep
  return finish(light)


# --- N4: plait rows ---------------------------------------------------------

def n4_plait():
  """Three cosine strands a third of a period apart. At every crossing the
  strand heading toward the plait's centre line lies on top, so the outer
  strand crosses over the middle one and each strand alternates."""
  w, amp, period, rh = 3.0, 4.0, 24.0, 12.0
  k = 2 * math.pi / period
  row = np.round((V - rh / 2) / rh)
  y = V - rh * row - rh / 2
  x = U - COLUMN_U

  def f(t, i, n=0):
    ph = k * t - 2 * math.pi * i / 3
    return [amp * np.cos(ph), -amp * k * np.sin(ph), -amp * k * k * np.cos(ph)][n]

  dist = []
  for i in range(3):
    t = x.copy()
    for _ in range(5):
      g = (t - x) + (f(t, i) - y) * f(t, i, 1)
      h = 1 + f(t, i, 1) ** 2 + (f(t, i) - y) * f(t, i, 2)
      t = t - np.clip(g / np.maximum(h, 0.2), -2, 2)
    dist.append(np.hypot(t - x, f(t, i) - y))
  light = np.zeros(U.shape, bool)
  for i in range(3):
    keep = dist[i] < w / 2
    for j in range(3):
      if j != i:
        th = math.pi * (i + j) / 3
        xc = (th + np.round((k * x - th) / math.pi) * math.pi) / k
        yc = f(xc, i)
        under = yc * f(xc, i, 1) > 0
        keep &= ~(under & (dist[j] < w / 2 + SEAM) & (np.hypot(x - xc, y - yc) < 1.5 * w))
    light |= keep
  return finish(light)


# --- N5: triaxial weave -----------------------------------------------------

def n5_triaxial():
  """Kagome: the three offsets sum to half a pitch, so no three bands meet.
  Family 0 lies over 1, 1 over 2, 2 over 0; along any band the crossings
  alternate between the other two families, so over and under alternate."""
  return straight_weave([(0, 12.0, 6.0), (120, 12.0), (240, 12.0)], 3.5,
                        lambda b, kb, a, ka: np.full(U.shape, a == (b + 1) % 3))


def n5a_triaxial():
  """N5 at two thirds scale; the seam stays 0.8 mm."""
  return straight_weave([(0, 8.0, 4.0), (120, 8.0), (240, 8.0)], 2.4,
                        lambda b, kb, a, ka: np.full(U.shape, a == (b + 1) % 3))


def n5b_triaxial():
  """N5a at half scale; the seam drops to 0.6 mm, the dark-gap print floor."""
  return straight_weave([(0, 4.0, 2.0), (120, 4.0), (240, 4.0)], 1.2,
                        lambda b, kb, a, ka: np.full(U.shape, a == (b + 1) % 3), seam=0.6)


CANDIDATES = [
  ('N0', n0_basket,
   'basket weave, bands 4 mm on a 6 mm pitch, plain over-under',
   'flat bands; each under band stops 0.8 mm short of the band on top'),
  ('N1', n1_lattice,
   'diagonal lattice, bands 4 mm at 45 and 135 degrees, 8.49 mm pitch, crossings on the 6 mm grid',
   'flat bands; plain over-under with 0.8 mm seams'),
  ('N2', n2_chain,
   'crochet chain rows 6 mm apart: stadium links on an 8 x 4 mm centre line with a 1.2 mm band, '
   '6 mm apart along the row, alternate rows shifted 3 mm',
   'flat links; each passes under the next at the top crossing and over it at the bottom, '
   '0.8 mm seams'),
  ('N3', n3_knit,
   'knit, U loops 8 mm wide in rows 6 mm apart, alternate rows shifted 4 mm, band 2 mm',
   'flat bands; each U head lies over the legs below, each leg ends under the head above'),
  ('N4', n4_plait,
   'plait rows 12 mm apart, three cosine strands 3 mm wide, 24 mm period, 1 mm dark line between rows',
   'flat bands; the outer strand crosses over the middle one, 0.8 mm seams'),
  ('N5', n5_triaxial,
   'triaxial weave, bands 3.5 mm at 90, 30 and 150 degrees on a 12 mm pitch, kagome layout',
   'flat bands; cyclic over-under, each band alternating, 0.8 mm seams'),
  ('N5a', n5a_triaxial,
   'tighter triaxial weave, bands 2.4 mm at 90, 30 and 150 degrees on an 8 mm pitch, kagome layout',
   'flat bands; cyclic over-under, each band alternating, 0.8 mm seams'),
  ('N5b', n5b_triaxial,
   'way tighter triaxial weave, bands 1.2 mm at 90, 30 and 150 degrees on a 4 mm pitch, kagome layout',
   'flat bands; cyclic over-under, each band alternating, 0.6 mm seams'),
]


def main(names):
  cb.OUT.mkdir(parents=True, exist_ok=True)
  for name, fn, what, how in CANDIDATES:
    if not names or name in names:
      cb.render(name, fn)


if __name__ == '__main__':
  main(sys.argv[1:])
