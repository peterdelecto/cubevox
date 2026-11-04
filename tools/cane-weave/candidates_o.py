#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family O: paisley, houndstooth, tweed.

Flat two-tone motifs on whole-pixel repeats, so every copy is the same
pixels. Nothing is shaded. Same face grid and report as candidates_b.

  candidates_o.py           every candidate as <name>.png
  candidates_o.py O0 O2     only the named candidates
"""

import math
import sys

import numpy as np

import candidates_b as cb
from candidates_b import ALLOWED, COLUMN_U, H, S, U, V, W, disc, dilate, erode, opening
from candidates_n import finish


def unit(deg):
  a = math.radians(deg)
  return np.array([math.cos(a), math.sin(a)])


def angle(p):
  return math.degrees(math.atan2(p[1], p[0]))


def arc(c, r, a0, a1, step=0.05):
  """Points on the circle (c, r) from a0 to a1 degrees, a1 excluded, about `step` mm apart."""
  n = max(2, int(abs(math.radians(a1 - a0)) * r / step))
  t = np.radians(np.linspace(a0, a1, n, endpoint=False))
  return np.stack([c[0] + r * np.cos(t), c[1] + r * np.sin(t)], axis=1)


def polygon_sdf(pts, x, y):
  """Signed distance, mm, from the points (x, y) to the closed polygon pts; negative inside."""
  d2 = np.full(x.shape, np.inf)
  inside = np.zeros(x.shape, bool)
  for (ax, ay), (bx, by) in zip(pts, np.roll(pts, -1, axis=0)):
    ex, ey = bx - ax, by - ay
    t = np.clip(((x - ax) * ex + (y - ay) * ey) / (ex * ex + ey * ey), 0, 1)
    d2 = np.minimum(d2, (x - ax - t * ex) ** 2 + (y - ay - t * ey) ** 2)
    if ay != by:
      crosses = ((ay > y) != (by > y)) & (x < ax + (y - ay) * ex / ey)
      inside ^= crosses
  return np.where(inside, -1.0, 1.0) * np.sqrt(d2)


# --- O0: paisley ------------------------------------------------------------

def boteh_outline():
  """Closed boteh outline, counter-clockwise, mm, y up, body centre at the
  origin. Five arcs, each tangent to the next: the body circle, a long arc
  up the back, the curl over the top, a 0.6 mm round tip, and the concave
  notch the curl wraps round, which meets the body tangentially."""
  R = 5.0                                # body
  rb, tb = 3.0, 60.0                     # notch radius; it touches the body at 60 degrees
  w, e, psi = 1.5, 1.8, 135.0            # curl: notch radius + w, centre e mm off toward psi
  ra = 14.0                              # back
  rho = 0.6                              # tip
  cb_ = (R + rb) * unit(tb)
  cc = cb_ + e * unit(psi)
  rc = rb + w

  # The back is tangent inside the curl circle and outside the body circle.

  a, b = ra - rc, ra - R
  dc = np.linalg.norm(cc)
  phi_a = angle(cc) + math.degrees(math.acos((dc * dc + a * a - b * b) / (2 * a * dc)))
  ca = cc - a * unit(phi_a)
  th_a = angle(-ca)

  # Tip: a circle of radius rho inside the curl circle and outside the notch.

  r1, r2 = rc - rho, rb + rho
  d = np.linalg.norm(cb_ - cc)
  along = (r1 * r1 - r2 * r2 + d * d) / (2 * d)
  off = math.sqrt(r1 * r1 - along * along)
  ax = (cb_ - cc) / d
  perp = np.array([-ax[1], ax[0]])
  f = max((cc + along * ax + s * off * perp for s in (1, -1)), key=lambda p: p[0])
  a_fb = angle(f - cb_)
  a_fc = angle(f - cc)

  def ccw(a0, a1):
    return a1 if a1 > a0 else a1 + 360

  parts = [
    arc(np.zeros(2), R, th_a, ccw(th_a, tb)),             # body, round the bottom
    arc(cb_, rb, tb + 180, a_fb if a_fb < tb + 180 else a_fb - 360),   # notch, clockwise
    arc(f, rho, a_fb + 180, ccw(a_fb + 180, a_fc)),       # tip
    arc(cc, rc, a_fc, ccw(a_fc, phi_a)),                  # curl, over the top
    arc(ca, ra, phi_a, ccw(phi_a, th_a)),                 # back, down to the body
  ]

  # Turned 12 degrees counter-clockwise so the curl sits over the body and
  # the motif stands upright.

  c, s = math.cos(math.radians(12)), math.sin(math.radians(12))
  return np.concatenate(parts) @ np.array([[c, s], [-s, c]])


def boteh_mask():
  """The boteh as light pixels: a 1.0 mm outline band inside the edge, a
  0.9 mm gap, a closed 1.0 mm inner outline, a 0.9 mm gap, then a solid
  light centre. Where the tail narrows, each dark core and the inner
  outline end in a round cap instead of a taper; the tail tip is solid.
  Local raster at the face's pixel pitch; the anchor (body centre) sits on
  a pixel corner at the middle column, so a left-right flip is the exact
  mirror. Returns the mask and the anchor's row and column."""
  half_w, top, rows = 100, 150, 230               # px: 10 mm each side, 15 mm above the anchor
  x = (np.arange(2 * half_w) - half_w + 0.5) / S
  y = (top - np.arange(rows) - 0.5) / S
  xx, yy = np.meshgrid(x, y)
  depth = -polygon_sdf(boteh_outline(), xx, yy)
  core = opening(depth >= 1.0, disc(10))
  inner = opening(depth >= 1.9, disc(20))
  hollow = opening(erode(inner, disc(20)), disc(10))
  centre = opening(depth >= 3.8, disc(10))
  light = ((depth >= 0) & ~core) | (inner & ~hollow) | centre
  return light, top, half_w


def o0_paisley():
  """Boteh 18.1 mm tall, 10.5 mm wide, in columns 12 mm apart with a 20 mm
  repeat down each column; each column drops 10 mm and mirrors its
  neighbour, so the curl alternates left and right. Copies sit 1.9 to
  2.7 mm apart. Anchors fall on the hole columns."""
  mask, top, half_w = boteh_mask()
  mirror = mask[:, ::-1]
  du, dv = 120, 200
  u0 = round(COLUMN_U * S)
  light = np.zeros(U.shape, bool)
  for c in range(-2, W // du + 3):
    for r in range(-2, H // dv + 3):
      pu = u0 + (c - 1) * du
      pv = r * dv + (c % 2) * dv // 2
      m = mirror if c % 2 else mask
      j0, i0 = pv - top, pu - half_w
      j1, i1 = j0 + m.shape[0], i0 + m.shape[1]
      cj0, ci0, cj1, ci1 = max(j0, 0), max(i0, 0), min(j1, H), min(i1, W)
      if cj0 < cj1 and ci0 < ci1:
        light[cj0:cj1, ci0:ci1] |= m[cj0 - j0:cj1 - j0, ci0 - i0:ci1 - i0]
  return light


# --- O1: houndstooth ----------------------------------------------------------

def o1_houndstooth():
  """2/2 twill, four dark and four light threads each way, cell 1.2 mm:
  teeth 4.8 mm, repeat 9.6 mm. A cell shows its warp colour where the warp is
  up, else its weft colour. No corner rounding, which would erase the 0.85 mm
  diagonal spurs; the print pass is the only trim."""
  cell = 1.2
  i = np.floor((U - COLUMN_U) / cell).astype(np.int64)
  j = np.floor(V / cell).astype(np.int64)
  warp_up = (i + j) % 4 < 2
  return opening(np.where(warp_up, i % 8 >= 4, j % 8 >= 4) & ALLOWED, disc(6))


# --- O2: tweed ------------------------------------------------------------------

def o2_tweed():
  """2/2 twill: rows 1.7 mm apart, each a run of light floats 2.4 x 1.0 mm
  with 0.45 mm corners, 4.8 mm apart. Each row steps one 1.2 mm cell from
  the last, so the twill runs on a diagonal. Every 7 rows (11.9 mm) the step
  reverses with a two-cell break, so the floats on either side of the turn
  face each other's gaps: herringbone."""
  pitch, length, height, cell, rad, band = 1.7, 2.4, 1.0, 1.2, 0.45, 7
  v0 = 0.8
  r = np.round((V - v0) / pitch).astype(np.int64)
  vc = v0 + r * pitch
  b = np.floor_divide(r, band)
  t = r - b * band
  phase = np.where(b % 2 == 0, t, band + 1 - t)
  first = COLUMN_U + (phase + 1) * cell
  uc = first + np.round((U - first) / (4 * cell)) * 4 * cell
  qx = np.maximum(np.abs(U - uc) - (length / 2 - rad), 0)
  qy = np.maximum(np.abs(V - vc) - (height / 2 - rad), 0)
  return np.hypot(qx, qy) < rad


CANDIDATES = [
  ('O0', o0_paisley),
  ('O1', o1_houndstooth),
  ('O2', o2_tweed),
]


def main(names):
  cb.OUT.mkdir(parents=True, exist_ok=True)

  # Within 1.5 mm of a hole ring, the window recess or the field edge.

  edge = dilate(~ALLOWED, disc(30))
  for name, fn in CANDIDATES:
    if not names or name in names:
      drawn = {}

      def keep(fn=fn):
        drawn['light'] = fn() & ALLOWED
        return drawn['light']

      img, _ = cb.render(name, keep)
      lit = np.all(np.asarray(img) == cb.LIGHT, axis=2)
      changed = lit != drawn['light']
      print(f'  light covers {100 * np.count_nonzero(lit) / np.count_nonzero(ALLOWED):.1f} % of the allowed face; '
            f'{100 * np.count_nonzero(changed & edge) / max(1, np.count_nonzero(changed)):.0f} % of the '
            f'print-pass change lies within 1.5 mm of a cut edge')


if __name__ == '__main__':
  main(sys.argv[1:])
