#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family L: woven and crocheted yarn in relief.

Each candidate builds a height field of round yarn in mm. A strand has a
half-round cross-section and dips smoothly where it passes under another.
The field is lit from the upper left at 40 degrees, darkened in its contact
shadows, and the lit half of the yarn becomes the light inlay. The ground
between strands stays dark. Same face grid and report as candidates_b.

  candidates_l.py           every candidate as <name>.png
  candidates_l.py L0 L3     only the named candidates
"""

import math
import sys

import numpy as np

import candidates_b as cb
from candidates_b import ALLOWED, COLUMN_U, H, S, U, V, W, blur, equalize
from candidates_c import cast_shadow, light_dir, lit, shift

LIGHT = light_dir(225, 40)
SQ2 = math.sqrt(2)


# --- Shared tools ---------------------------------------------------------

def ramp_up(x):
  """Cosine step, 0 below x = 0, 1 above x = 1."""
  return 0.5 * (1 - np.cos(np.pi * np.clip(x, 0, 1)))


def bump(d, core, ramp):
  """1 within `core` of zero, falling to 0 by a cosine over the next `ramp`."""
  return 1 - ramp_up((np.abs(d) - core) / ramp)


def circle3(a, b, c):
  """Centre and radius of the circle through three points."""
  (ax, ay), (bx, by), (cx, cy) = a, b, c
  den = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
  a2, b2, c2 = ax * ax + ay * ay, bx * bx + by * by, cx * cx + cy * cy
  ux = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / den
  uy = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / den
  return ux, uy, math.hypot(ax - ux, ay - uy)


def wrap(a):
  return (a + np.pi) % (2 * np.pi) - np.pi


def arc_dist(x, y, p0, pm, p1):
  """Distance from each point to the circular arc p0 -> pm -> p1 (round ends),
  and the arc parameter of the nearest point, 0 at p0 and 1 at p1."""
  cx, cy, r = circle3(p0, pm, p1)
  am = math.atan2(pm[1] - cy, pm[0] - cx)
  a0 = float(wrap(math.atan2(p0[1] - cy, p0[0] - cx) - am))
  a1 = float(wrap(math.atan2(p1[1] - cy, p1[0] - cx) - am))
  a = np.clip(wrap(np.arctan2(y - cy, x - cx) - am), min(a0, a1), max(a0, a1))
  d = np.hypot(x - cx - r * np.cos(a + am), y - cy - r * np.sin(a + am))
  return d, (a - a0) / (a1 - a0)


def seg_dist(x, y, p0, p1):
  """Distance to the segment p0 -> p1 (round ends) and the parameter along it."""
  dx, dy = p1[0] - p0[0], p1[1] - p0[1]
  t = np.clip(((x - p0[0]) * dx + (y - p0[1]) * dy) / (dx * dx + dy * dy), 0, 1)
  return np.hypot(x - p0[0] - t * dx, y - p0[1] - t * dy), t


def nearest_of(c, period, k, n):
  """Index of the nearest line c = period * i with i = k (mod n)."""
  return n * np.round((c / period - k) / n) + k


class Relief:
  """Height field of yarn over a flat ground at 0 mm, with the slope of the
  strand on top at each pixel. Each strand's slope comes from its own field,
  so a strand ending on another leaves no step artefact."""

  def __init__(self):
    self.h = np.zeros((H, W))
    self.gu = np.zeros((H, W))
    self.gv = np.zeros((H, W))
    self.yarn = np.zeros((H, W), bool)

  def add(self, raw, inside):
    gv, gu = np.gradient(raw, 1 / S)
    take = inside & (raw > self.h)
    self.h[take], self.gu[take], self.gv[take] = raw[take], gu[take], gv[take]
    self.yarn |= take

  def strand(self, z, d, r):
    """A half-round strand of radius r, centre line at height z, d mm away."""
    self.add(z + np.sqrt(np.clip(r * r - d * d, 0, None)), d < r)


def occlusion(h, reach=1.5, rise=0.4):
  """Share of samples within `reach` mm that stand more than `rise` mm higher."""
  hits = np.zeros(h.shape)
  n = 0
  for rad in (reach / 3, 2 * reach / 3, reach):
    for k in range(12):
      a = 2 * math.pi * (k + 0.5 * (rad > reach / 2)) / 12
      hits += shift(h, round(rad * S * math.sin(a)), round(rad * S * math.cos(a))) > h + rise
      n += 1
  return hits / n


def two_tone(rel, thr=0.5):
  """Light where the lit, unoccluded yarn ranks above `thr` within the yarn."""
  c = lit(rel.gu, rel.gv, LIGHT)
  c = c * np.clip(1 - 2.0 * occlusion(rel.h), 0, 1)
  c = c * np.where(cast_shadow(rel.h, LIGHT, 2.5), 0.45, 1.0)
  c = blur(np.where(rel.yarn, c, 0.0), 0.1)
  where = rel.yarn & ALLOWED
  return (equalize(c, where) > thr) & where


# --- L0: cane webbing -----------------------------------------------------

def l0_cane():
  p, off, rp, rd = 12.0, 1.55, 1.25, 1.1
  dip, lift, ramp, ramp_d = 1.6, 0.5, 1.0, 0.5
  band = off + rp

  # Weave coordinates as weave.proof(): band lines on multiples of P, y up.

  xw, yw = U + 18.0, 154.5 - V
  j = np.round(xw / p)
  k = np.round(yw / p)
  ex, ey = xw - j * p, yw - k * p
  h_over = (j + k) % 2 == 0
  s1 = (yw - xw) / p - 0.5
  s2 = (yw + xw) / p - 0.5
  a, b = np.round(s1), np.round(s2)
  d1, d2 = (s1 - a) * p / SQ2, (s2 - b) * p / SQ2
  back_over = (b - a) % 2 == 0

  under_diag = bump(d1, rd + 0.1, ramp_d) + bump(d2, rd + 0.1, ramp_d)
  on_band = np.maximum(bump(ex, band, ramp), bump(ey, band, ramp))
  rel = Relief()
  zh = np.where(h_over, lift * bump(ex, band, ramp), -dip * bump(ex, band + 0.2, ramp))
  rel.strand(zh - dip * under_diag, np.abs(np.abs(ey) - off), rp)
  zv = np.where(h_over, -dip * bump(ey, band + 0.2, ramp), lift * bump(ey, band, ramp))
  rel.strand(zv - dip * under_diag, np.abs(np.abs(ex) - off), rp)
  z1 = np.where(back_over, -dip * bump(d2, rd + 0.1, ramp_d), lift * bump(d2, rd, ramp_d))
  rel.strand(lift * on_band + z1, np.abs(d1), rd)
  z2 = np.where(back_over, lift * bump(d1, rd, ramp_d), -dip * bump(d1, rd + 0.1, ramp_d))
  rel.strand(lift * on_band + z2, np.abs(d2), rd)
  return two_tone(rel, 0.46)


# --- L1: herringbone twill ------------------------------------------------

def l1_twill():
  pitch, r, dip, lift = 3.2, 1.3, 2.2, 0.5
  u0 = COLUMN_U

  def warp_over(i, j):
    rev = np.floor((u0 + i * pitch) / 24.0) % 2 == 1
    return np.where(rev, (i - j) % 4, (i + j) % 4) < 2

  def along(over, s):
    """Centre-line height s crossings along the strand. A cos^2 kernel 3
    crossings wide sums to 1.5 everywhere, so each float arches smoothly,
    highest at its middle, and the yarn tapers where it sinks."""
    k0 = np.floor(s)
    fr = s - k0
    a = np.zeros(s.shape)
    for m in (-1, 0, 1, 2):
      x = fr - m
      a += over(k0 + m) * np.where(np.abs(x) < 1.5, np.cos(np.pi * x / 3) ** 2, 0) / 1.5
    return -dip + (dip + lift) * a

  rel = Relief()
  fu, fv = (U - u0) / pitch, V / pitch
  i, j = np.round(fu), np.round(fv)
  rel.strand(along(lambda k: warp_over(i, k), fv), np.abs(fu - i) * pitch, r)
  rel.strand(along(lambda k: ~warp_over(k, j), fu), np.abs(fv - j) * pitch, r)
  return two_tone(rel, 0.5)


# --- L2: stockinette knit -------------------------------------------------

def l2_knit():
  r, dip, cw, rh = 1.2, 2.6, 8.0, 6.0
  leg = ((-0.9, 2.9), (-2.4, 0.0), (-3.0, -3.0))    # left leg: V point, bulge, top
  c = np.round((U - COLUMN_U) / cw)
  x = U - COLUMN_U - c * cw
  rel = Relief()
  for k in range(3):
    row = nearest_of(V - rh / 2, rh, k, 3)
    y = V - rh * (row + 0.5)
    for sx in (1, -1):
      d, t = arc_dist(sx * x, y, *leg)
      z = 0.3 * (1 - ramp_up(t / 0.3)) - dip * ramp_up((t - 0.72) / 0.28)
      rel.strand(z, d, r)
  return two_tone(rel, 0.45)


# --- L3: single crochet ---------------------------------------------------

def l3_crochet():
  r, dip, link, rh, top = 1.2, 2.4, 6.0, 7.0, 2.6
  leg = ((0.3, -1.25), (2.4, -1.35), (5.6, 0.0))    # upper leg: open end, bulge, tip
  u0 = COLUMN_U - link / 2
  rel = Relief()
  for k in range(2):
    row = nearest_of(V - top - 2.15, rh, k, 2)
    y = V - (rh * row + top)
    for q in range(2):
      lk = nearest_of(U - u0 - 3.15, link, q, 2)
      x = U - u0 - link * lk
      for sy in (1, -1):
        d, t = arc_dist(x, sy * y, *leg)
        rel.strand(0.4 * (1 - ramp_up(t / 0.35)) - dip * ramp_up((t - 0.65) / 0.35), d, r)
      for px in (1.5, 4.5):
        d, t = seg_dist(x, y, (px, 1.5), (px, 5.5))
        z = -dip * np.maximum(ramp_up((0.35 - t) / 0.35), ramp_up((t - 0.65) / 0.35))
        rel.strand(z, d, r)
  return two_tone(rel, 0.5)


# --- L4: rope macrame -----------------------------------------------------

def l4_macrame():
  big, lay, slant, groove, flat = 3.0, 3.0, 0.4, 1.3, 0.7
  ku0, kv0, pitch = COLUMN_U, 4.25, 24.0          # one knot centre; knots every 24 mm
  qa = (U + V - (ku0 + kv0)) / pitch              # "/" ropes
  qb = (U - V - (ku0 - kv0)) / pitch              # "\" ropes
  a, b = np.round(qa), np.round(qb)
  ku = ku0 + pitch * (a + b) / 2
  kv = kv0 + pitch * (a - b) / 2
  kx, ky = U - ku, V - kv
  near_knot = bump(np.hypot(kx, ky), 2.5, 3.0)
  a_over = (a + b) % 2 == 0
  ca, cb_ = (qa - a) * pitch / SQ2, (qb - b) * pitch / SQ2

  rel = Relief()
  for across, other, over, run in ((ca, cb_, a_over, (U - V) / SQ2),
                                   (cb_, ca, ~a_over, (U + V) / SQ2)):
    z = -1.4 * near_knot + np.where(over, 0.0, -1.6 * bump(other, big, 2.0))
    ph = (run + slant * across) / lay
    lobe = np.sqrt(np.clip(1 - (2 * (ph - np.round(ph))) ** 2, 0, None))
    base = flat * np.sqrt(np.clip(big * big - across * across, 0, None))
    raw = z + base - groove * (1 - lobe)
    rel.add(raw, (np.abs(across) < big) & (raw > 0))

  # Square knot: two bowed bars across the crossing, ends tucked, and a side
  # loop each side.

  bar = ((-4.5, -0.8), (0.0, -2.2), (4.5, -0.8))
  loop = ((-4.6, -1.5), (-6.2, 0.0), (-4.6, 1.5))
  for sy in (1, -1):
    d, t = arc_dist(kx, sy * ky, *bar)
    rel.strand(1.8 - 2.6 * ramp_up((np.abs(t - 0.5) - 0.3) / 0.2), d, 1.6)
  for sx in (1, -1):
    d, t = arc_dist(sx * kx, ky, *loop)
    rel.strand(0.8 + 0.3 * bump(t - 0.5, 0.2, 0.3), d, 1.3)
  return two_tone(rel, 0.5)


CANDIDATES = [
  ('L0', l0_cane,
   'cane webbing in relief: paired horizontal and vertical strands 2.5 mm wide, 0.6 mm apart, '
   'on a 12 mm band pitch, single diagonals 2.2 mm wide through the band midpoints, octagonal '
   'holes, over-and-under as weave.py',
   'half-round yarn lit from the upper left at 40 degrees; the lit flank of each strand is '
   'light; under strands dip 1.6 mm beside the strand on top, which rises 0.5 mm, so the '
   'light band narrows and breaks at every crossing; contact shadow and cast shadow darken '
   'the strand underneath'),
  ('L1', l1_twill,
   'herringbone twill, yarn 2.6 mm on a 3.2 mm pitch, 2 over 2 under, the diagonal '
   'reversing every 24 mm between the hole columns',
   'each float shows a light band on its upper or left flank, about 7 mm long, fading where '
   'it dives; the bands step diagonally and turn at each reversal, so they make chevrons'),
  ('L2', l2_knit,
   'stockinette knit, columns of V stitches 8 mm wide by 6 mm tall on the hole columns, '
   'yarn 2.4 mm',
   'each V is two bowed legs whose tops dive under the point of the V above; the left flank '
   'of each leg is light, the inner flank and the tucks are dark'),
  ('L3', l3_crochet,
   'single crochet rows 7 mm tall: a chain braid of sideways V links 6 mm long along the top '
   'of each row, yarn 2.4 mm, two short vertical posts under each link',
   'each braid leg is light along its upper flank and dark where its tip tucks into the next '
   'link; the posts show a light left flank between the braids'),
  ('L4', l4_macrame,
   'three-strand ropes 6 mm thick on a 24 mm diamond lattice, a square knot at each crossing',
   'the rope twist makes slanted lobes 3 mm apart along each rope, light on their upper-left '
   'flank; the knots are two bowed bars and two side loops standing above the ropes, which '
   'dip beneath them and take their shadow'),
]


def main(names):
  cb.OUT.mkdir(parents=True, exist_ok=True)
  for name, fn, what, how in CANDIDATES:
    if not names or name in names:
      cb.render(name, fn)


if __name__ == '__main__':
  main(sys.argv[1:])
