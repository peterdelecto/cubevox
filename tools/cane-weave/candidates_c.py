#!/usr/bin/env python3
"""Candidate lid-face inlay textures, material C: crumpled paper.

Each candidate models a crumpled sheet as a height field of flat facets that
meet at sharp creases, lights it, and draws the light as printable two-tone
marks. The face grid, the hole geometry, the printability pass and the output
images come from candidates_b.py. Each candidate function returns the
light-inlay mask on the face grid: 10 px/mm, 1920 x 1520 px, u right and v
down from the face's top-left corner.

  candidates_c.py           every candidate's face PNG; prints the report lines
  candidates_c.py C0 C3     only the named candidates
"""

import math
import random
import sys

import numpy as np
from scipy.spatial import cKDTree

from candidates_b import (ALLOWED, COLUMN_U, H, OPEN_DISC, OUT, S, U, V, W, blur, dilate, disc,
                          equalize, hash01, line_dist, line_marks, opening, render, wave_noise)
from candidates_g import disc_offsets, scatter, stamp


# --- Crumpled sheet ---------------------------------------------------------

def delaunay(pts):
  """Bowyer-Watson triangulation of points (n, 2); returns (m, 3) index rows."""
  n = len(pts)
  mid, span = pts.mean(0), 20 * np.ptp(pts, 0).max()
  allp = np.vstack([pts, mid + [-span, -span], mid + [span, -span], mid + [0, span]])
  x, y = allp[:, 0], allp[:, 1]

  def circles(t):
    ax, ay, bx, by, cx, cy = x[t[:, 0]], y[t[:, 0]], x[t[:, 1]], y[t[:, 1]], x[t[:, 2]], y[t[:, 2]]
    d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
    a2, b2, c2 = ax * ax + ay * ay, bx * bx + by * by, cx * cx + cy * cy
    ux = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d
    uy = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d
    return np.stack([ux, uy, (ax - ux) ** 2 + (ay - uy) ** 2], 1)

  tris = np.array([[n, n + 1, n + 2]])
  circ = circles(tris)
  for p in range(n):
    bad = (circ[:, 0] - x[p]) ** 2 + (circ[:, 1] - y[p]) ** 2 < circ[:, 2]
    edges = np.sort(tris[bad][:, [0, 1, 1, 2, 2, 0]].reshape(-1, 2), 1)
    uniq, count = np.unique(edges, axis=0, return_counts=True)
    rim = uniq[count == 1]
    new = np.column_stack([rim, np.full(len(rim), p)])
    tris = np.vstack([tris[~bad], new])
    circ = np.vstack([circ[~bad], circles(new)])
  return tris[(tris < n).all(1)]


class Sheet:
  """A crumpled sheet of flat triangular facets. One point falls at random in
  each square of a `cell` mm grid (spread over `jitter` of the square, the
  grid stretched by `aspect` across and down); the points join into
  triangles, and each point is raised 0 to `amp` mm plus `shape(u, v)` if
  given. Each facet is a plane, each edge a crease.

  Per pixel: h (mm), the slope gu, gv, and fid, the facet it lies in.
  Per facet: g, its slope; nbr and kink, for each of its three edges the
  facet across it and the change of slope across it, positive for a valley
  and negative for a ridge."""

  def __init__(self, cell, amp, seed, jitter=0.9, shape=None, aspect=(1.0, 1.0)):
    rng = np.random.default_rng(seed)
    cu, cv = cell * aspect[0], cell * aspect[1]
    gi, gj = np.meshgrid(np.arange(-2, W / S / cu + 2), np.arange(-2, H / S / cv + 2))
    pu = (gi.ravel() + 0.5 + jitter * (rng.random(gi.size) - 0.5)) * cu
    pv = (gj.ravel() + 0.5 + jitter * (rng.random(gi.size) - 0.5)) * cv
    pz = rng.uniform(0, amp, gi.size) + (0 if shape is None else shape(pu, pv))
    tris = delaunay(np.column_stack([pu, pv]))

    # Barycentric weights: lambda = [u, v, 1] @ w, and the plane h = [u, v, 1] @ plane.

    self.w = np.linalg.inv(np.stack([pu[tris], pv[tris], np.ones(tris.shape)], -1))
    plane = np.einsum('mij,mj->mi', self.w, pz[tris])
    self.g = plane[:, :2]

    # Edge r lies opposite vertex r; its outward normal is minus the
    # gradient of lambda_r, whose length is 1 over the facet's height above it.

    grad = self.w[:, :2, :]
    self.alt = 1 / np.hypot(grad[:, 0], grad[:, 1])
    self.nbr = np.full(tris.shape, -1)
    owner = {}
    for k, tri in enumerate(tris):
      for r in range(3):
        key = tuple(sorted((tri[(r + 1) % 3], tri[(r + 2) % 3])))
        if key in owner:
          k2, r2 = owner.pop(key)
          self.nbr[k, r], self.nbr[k2, r2] = k2, k
        else:
          owner[key] = (k, r)
    out_u, out_v = -grad[:, 0] * self.alt, -grad[:, 1] * self.alt
    jump = self.g[self.nbr] - self.g[:, None, :]
    self.kink = np.where(self.nbr >= 0, jump[..., 0] * out_u + jump[..., 1] * out_v, 0.0)

    self.fid = np.zeros(U.shape, np.int64)
    for k, (a, b, c) in enumerate(tris):
      tu, tv = pu[[a, b, c]], pv[[a, b, c]]
      i0, i1 = max(0, int(tu.min() * S)), min(W, int(tu.max() * S) + 2)
      j0, j1 = max(0, int(tv.min() * S)), min(H, int(tv.max() * S) + 2)
      if i0 >= i1 or j0 >= j1:
        continue
      uu, vv, wk = U[j0:j1, i0:i1], V[j0:j1, i0:i1], self.w[k]
      inside = np.ones(uu.shape, bool)
      for r in range(3):
        inside &= wk[0, r] * uu + wk[1, r] * vv + wk[2, r] >= -1e-9
      self.fid[j0:j1, i0:i1][inside] = k
    f = self.fid
    self.gu, self.gv = plane[f, 0], plane[f, 1]
    self.h = self.gu * U + self.gv * V + plane[f, 2]

  def edge(self, r):
    """For edge r of each pixel's facet: the distance to it (mm), its kink,
    and the facet across it."""
    f = self.fid
    lam = self.w[f, 0, r] * U + self.w[f, 1, r] * V + self.w[f, 2, r]
    return lam * self.alt[f, r], self.kink[f, r], self.nbr[f, r]


def fold_lines(count, length, reach, amp, seed, soft=False, ridge_share=0.5):
  """A sheet creased along `count` straight folds, each a segment of random
  length within `length` (mm) at a random place and angle. The sheet rises
  or sinks by up to `amp` mm toward each fold from `reach` mm away (both
  ranges); `ridge_share` of the folds rise. Sharp folds rise in flat planes
  to a knife edge; soft folds round over at the crest and ease out at the foot.
  Returns the height h (mm)."""
  rng = np.random.default_rng(seed)
  h = np.zeros(U.shape)
  for _ in range(count):
    cu, cv = rng.uniform(-10, W / S + 10), rng.uniform(-10, H / S + 10)
    half = rng.uniform(*length) / 2
    rho = rng.uniform(*reach)
    a = rng.uniform(*amp) * (1 if rng.random() < ridge_share else -1)
    th = rng.uniform(0, math.pi)
    ca, sa = math.cos(th), math.sin(th)
    ext = half + rho
    i0, i1 = max(0, int((cu - ext) * S)), min(W, int((cu + ext) * S) + 1)
    j0, j1 = max(0, int((cv - ext) * S)), min(H, int((cv + ext) * S) + 1)
    if i0 >= i1 or j0 >= j1:
      continue
    du, dv = U[j0:j1, i0:i1] - cu, V[j0:j1, i0:i1] - cv
    along = du * ca + dv * sa
    across = dv * ca - du * sa
    x = np.hypot(along - np.clip(along, -half, half), across) / rho
    p = np.cos(0.5 * math.pi * np.minimum(x, 1)) ** 2 if soft else np.clip(1 - x, 0, None)
    h[j0:j1, i0:i1] += a * p
  return h


def light_dir(azimuth, elevation):
  """Unit vector toward the light. Azimuth in degrees on the page, 0 toward
  the right and 90 toward the bottom, so 225 is the upper left."""
  a, e = math.radians(azimuth), math.radians(elevation)
  return np.array([math.cos(e) * math.cos(a), math.cos(e) * math.sin(a), math.sin(e)])


def lit(gu, gv, light):
  """Cosine between the surface normal and the light."""
  lu, lv, lz = light
  return np.clip((lz - lu * gu - lv * gv) / np.sqrt(1 + gu * gu + gv * gv), 0, 1)


def shift(a, dy, dx):
  """a sampled dy rows down and dx columns right of each pixel, edges held."""
  p = np.pad(a, ((abs(dy), abs(dy)), (abs(dx), abs(dx))), mode='edge')
  return p[abs(dy) + dy:abs(dy) + dy + a.shape[0], abs(dx) + dx:abs(dx) + dx + a.shape[1]]


def cast_shadow(h, light, reach):
  """Pixels whose ray toward the light meets the height field within `reach` mm."""
  lu, lv, lz = light
  run = math.hypot(lu, lv)
  out = np.zeros(h.shape, bool)
  seen = set()
  for k in range(1, int(reach * S) + 1):
    dx, dy = round(k * lu / run), round(k * lv / run)
    if (dx, dy) in seen:
      continue
    seen.add((dx, dy))
    out |= shift(h, dy, dx) > h + math.hypot(dx, dy) / S * lz / run
  return out


def tone(light_cos):
  """Rank of each pixel's light within the inlay field, 0 to 1."""
  return equalize(light_cos, ALLOWED)


def level_hatch(gu, gv, fid, period, seed):
  """Distance, mm, to the nearest of a set of lines `period` mm apart that
  run along each facet's level lines, so the angle turns at every crease;
  each facet gets its own phase."""
  g = np.hypot(gu, gv)
  flat = g < 1e-3
  nu = np.where(flat, 0.6, gu / np.maximum(g, 1e-9))
  nv = np.where(flat, 0.8, gv / np.maximum(g, 1e-9))
  return line_dist(U * nu + V * nv + period * hash01(fid, seed), period)


def crease_lines(sheet, light, share, narrow, wide):
  """Dark lines along the sharpest `share` of the creases, `narrow` mm wide
  at the weakest drawn up to `wide` mm at the sharpest. A valley's line
  runs down its middle; a ridge's lies wholly on its shaded side, as the
  shadow the ridge throws."""
  facet_lit = lit(sheet.g[:, 0], sheet.g[:, 1], light)
  kinks = np.abs(sheet.kink[sheet.nbr >= 0])
  k0, k1 = np.quantile(kinks, 1 - share), np.quantile(kinks, 0.98)
  out = np.zeros(U.shape, bool)
  for r in range(3):
    d, k, n = sheet.edge(r)
    width = narrow + (wide - narrow) * np.clip((np.abs(k) - k0) / (k1 - k0), 0, 1)
    drawn = np.abs(k) >= k0
    valley = drawn & (k > 0) & (d < width / 2)
    shaded = facet_lit[sheet.fid] < facet_lit[n]
    ridge = drawn & (k < 0) & shaded & (d < width)
    out |= valley | ridge
  return out


# --- C0: sheet crumpled and smoothed back -------------------------------------

def c0_smoothed():
  """A sheet crumpled and smoothed flat again: low relief, facets 3 to 8 mm
  across on a broad swell, and a web of fine creases. Light from the upper
  left. The paper is solid light; the sharpest 60 % of creases are dark
  lines 0.7 to 1.2 mm wide, down the middle of a valley and on the shaded
  side of a ridge; the facets turned furthest from the light carry a hatch
  along their level lines that thins into shadow."""
  sheet = Sheet(6.0, 0.6, 101, shape=lambda u, v: 1.5 * wave_noise(u, v, 103, 45))
  light = light_dir(225, 45)
  t = tone(lit(sheet.gu, sheet.gv, light))
  paper = (t >= 0.3) | (line_dist((U - V) / math.sqrt(2), 2.0) < 0.475)
  return paper & ~crease_lines(sheet, light, 0.6, 0.7, 1.1)


# --- C1: coarse crumple, big facets -------------------------------------------

def c1_coarse():
  """A coarse crumple: big facets 10 to 25 mm across. Light from the upper
  left. Every facet is hatched along its own level lines, so the line angle
  turns at each crease; the lines are 2.8 mm apart and swell from 0.9 mm in
  shadow to solid on the lit facets, and the facets in cast shadow are dark."""
  sheet = Sheet(18.0, 7.0, 111)
  light = light_dir(225, 40)
  t = tone(lit(sheet.gu, sheet.gv, light))
  t[cast_shadow(sheet.h, light, 6.0)] = 0
  return line_marks(level_hatch(sheet.gu, sheet.gv, sheet.fid, 2.8, 7), 2.8, t, lo=0.12, hi=0.86)


# --- C2: folded in a grid, then crumpled --------------------------------------

def c2_grid_folded():
  """A sheet folded on a 12 mm grid, mountain and valley in turn, the hole
  columns on mountain folds, so the folds repeat every 24 mm; then crumpled.
  Light from the upper left. Horizontal ruled lines 2.0 mm apart ride up
  over the relief, so they zigzag across the folds, kink at every crease
  and bunch or spread with each facet's tilt; each line swells on the lit
  facets and thins into shadow."""
  sheet = Sheet(9.0, 3.0, 121)
  zu = 1 - line_dist(U - COLUMN_U, 24.0) / 12.0
  zv = 1 - line_dist(V - 4.0, 24.0) / 12.0
  grid = 5.0 * zu + 2.5 * zv
  gu = np.gradient(grid, 1 / S, axis=1) + sheet.gu
  gv = np.gradient(grid, 1 / S, axis=0) + sheet.gv
  h = grid + sheet.h
  light = light_dir(235, 40)
  t = tone(lit(gu, gv, light))
  t[cast_shadow(h, light, 4.0)] = 0
  period, lift = 2.0, 0.9
  across = V - lift * h
  stretch = np.hypot(lift * gu, 1 - lift * gv)
  return line_marks(line_dist(across, period) / stretch, period / stretch, t, lo=0.03, hi=None)


# --- C3: tight ball, flattened -------------------------------------------------

def c3_balled():
  """A sheet balled tight and pressed flat: dense sharp folds 5 to 16 mm
  long, broken into facets 2 to 4 mm across. Low light from the left. Flat
  shading in three tones, like a woodcut: facets facing the light are solid
  light, facets half turned away carry horizontal lines 0.95 mm wide on a
  1.7 mm pitch, and facets turned away or in the long cast shadows are dark."""
  folds = fold_lines(900, (6.0, 20.0), (1.5, 4.0), (0.6, 1.4), 131)

  def at_points(u, v):
    return folds[np.clip((v * S).astype(int), 0, H - 1), np.clip((u * S).astype(int), 0, W - 1)]

  sheet = Sheet(3.2, 0.25, 132, shape=at_points)
  light = light_dir(210, 28)
  t = tone(lit(sheet.gu, sheet.gv, light))
  t[cast_shadow(sheet.h, light, 3.0)] = 0
  return (t > 0.5) & ~crease_lines(sheet, light, 0.35, 0.7, 0.7)


# --- C4: soft tissue crumple ---------------------------------------------------

def c4_tissue():
  """Tissue paper crumpled softly: rounded folds, facets about 5 to 10 mm.
  Light from the upper right. A stipple of dots scattered no closer than
  2.2 mm: light dots on dark grow from 0.9 to 1.5 mm with the light, then
  the ground turns light under dark dots that shrink from 1.3 to 0.7 mm,
  and the brightest crowns are solid."""
  h = blur(Sheet(12.0, 4.5, 141).h, 1.5)
  gv, gu = np.gradient(h, 1 / S)
  t = blur(tone(lit(gu, gv, light_dir(315, 35))), 0.3)
  return aquatint(t, 2.6, 142, lo=0.08, hi=0.92)


def aquatint(t, wavelength, seed, lo=0.06, hi=0.94):
  """A grain like an aquatint: light where a maze of noise, its waves all
  near `wavelength` mm, ranks below the tone. Coverage follows t smoothly,
  from bare dark under `lo` to solid light over `hi`."""
  rng = np.random.default_rng(seed)
  grain = np.zeros(U.shape)
  for _ in range(24):
    a = rng.uniform(0, 2 * math.pi)
    k = 2 * math.pi / (wavelength * rng.uniform(0.9, 1.1))
    grain += np.sin(k * (U * math.cos(a) + V * math.sin(a)) + rng.uniform(0, 2 * math.pi))
  return equalize(grain) < np.clip((t - lo) / (hi - lo), 0, 1)


# --- Contour bands -------------------------------------------------------------

def tissue():
  """C4's sheet: the soft crumple's height h (mm)."""
  return blur(Sheet(12.0, 4.5, 141).h, 1.5)


def tissue_tone(h):
  """C4's light: the rank of each pixel's light from the upper right, 0 to 1."""
  gv, gu = np.gradient(h, 1 / S)
  return blur(tone(lit(gu, gv, light_dir(315, 35))), 0.3)


def sample(a, u, v):
  """Field a read at (u, v) mm by bilinear interpolation, edges held."""
  x = np.clip(u * S - 0.5, 0, W - 1.001)
  y = np.clip(v * S - 0.5, 0, H - 1.001)
  i, j = x.astype(np.int64), y.astype(np.int64)
  fx, fy = x - i, y - j
  top = a[j, i] * (1 - fx) + a[j, i + 1] * fx
  bottom = a[j + 1, i] * (1 - fx) + a[j + 1, i + 1] * fx
  return top * (1 - fy) + bottom * fy


# Narrowest band and gap drawn, mm: the print limits plus margin for the
# first-order distance to a curved level line.

BAND_LIGHT = 1.0
BAND_DARK = 0.8


def phase_slope(phase, quantum=None):
  """Climb of `phase` (cycles) along u and along v, cycles per mm. With
  `quantum`, jumps by whole multiples of it, where an angle wraps, are
  ignored."""
  def diff(axis):
    a = np.moveaxis(phase, axis, 0)
    d = np.empty_like(a)
    d[1:-1] = a[2:] - a[:-2]
    d[0], d[-1] = 2 * (a[1] - a[0]), 2 * (a[-1] - a[-2])
    if quantum:
      d -= np.round(d / quantum) * quantum
    return np.moveaxis(d, 0, axis) * S / 2
  return diff(1), diff(0)


def contour_bands(phase, t, pitch, levels, lo, hi, beat=None, quantum=None):
  """Light bands along the whole-number level lines of `phase` (cycles;
  `quantum` as in phase_slope). Lines thin out where they bunch and split
  in where they spread, so the pitch stays near `pitch` mm (a number or a
  field): of each pair, one line fades to its narrowest and ends while its
  neighbour swells, through levels of every line, every second, and so on
  to every 2^(levels-1)-th. Width follows tone t, from BAND_LIGHT over
  `lo` to solid over `hi`, and keeps every gap at least BAND_DARK; where
  even the sparsest lines sit too close, light goes solid and shadow bare.
  `beat`, a pair (phase2, depth), swells and pinches the bands along the
  cycles of a second phase, most in the mid tones; a pinched band narrows
  to BAND_LIGHT and never breaks."""
  gu, gv = phase_slope(phase, quantum)
  rate = np.maximum(np.hypot(gu, gv), 1e-6)
  level = np.clip(blur(np.clip(np.log2(pitch * rate), -1, levels), 5.0), 0, levels - 1)
  j = np.minimum(np.floor(level), levels - 2)
  s = level - j
  step = 2.0 ** (j + 1)
  gap = step / 2 / rate
  k = np.clip((t - lo) / (hi - lo), 0, 1)
  k = k * k * (3 - 2 * k)
  if beat is not None:
    phase2, depth = beat
    k = np.clip(k + 4 * depth * k * (1 - k) * np.cos(2 * math.pi * phase2), 0, 1)

  def width(g):
    return np.where(t <= lo, 0.0, BAND_LIGHT + k * np.maximum(g - BAND_DARK - BAND_LIGHT, 0))

  fade = (1 - s) * width(gap)
  fade = np.where(fade < BAND_LIGHT, 0.0, fade)
  room = np.where(fade > 0, 2 * gap - 2 * BAND_DARK - fade, 2 * gap - BAND_DARK)
  keep = np.minimum((1 - s) * width(gap) + s * width(2 * gap), room)
  d_keep = line_dist(phase / step, 1) * 2 * gap
  d_fade = line_dist(phase / step - 0.5, 1) * 2 * gap
  light = (d_keep < keep / 2) | (d_fade < fade / 2) | (t >= hi)
  tight = room < BAND_LIGHT
  return np.where(tight, t > 0.5, light)


def crowns(h, reach, count):
  """The `count` highest local peaks of h, each the top within `reach` mm,
  as (u, v) mm."""
  c = h[5::S, 5::S]
  n = int(reach)
  p = np.pad(c, n, mode='constant', constant_values=-np.inf)
  top = np.ones(c.shape, bool)
  for dy in range(-n, n + 1):
    for dx in range(-n, n + 1):
      if (dy or dx) and dy * dy + dx * dx <= n * n:
        top &= c >= p[n + dy:n + dy + c.shape[0], n + dx:n + dx + c.shape[1]]
  jj, ii = np.nonzero(top)
  best = np.argsort(-c[jj, ii])[:count]
  return [(ii[k] + 0.55, jj[k] + 0.55) for k in best]


# --- C5: tissue drawn in contour bands ----------------------------------------

def c5_contours():
  """C4's tissue drawn as a contour map: light bands follow the level lines
  of the height every 0.1 mm of rise, thinned to every second, fourth or
  eighth line where they bunch, so the pitch stays near 3.2 mm. Each band
  swells with the light from the upper right, from 1.0 mm on the shaded
  slopes to solid on the brightest crowns, and the deepest shadow is bare."""
  h = tissue()
  t = tissue_tone(h)
  hb = blur(h, 2.0)
  dh = 0.1
  return contour_bands(hb / dh, t, 3.2, 4, lo=0.15, hi=0.88)


# --- C6: swirled tissue, two band families in moire ----------------------------

def swirl(centres, radius, turn, su=U, sv=V):
  """Where each pixel reads the sheet from, (u, v) mm, under a warp that
  turns it about each centre by `turn` radians at the centre, fading over
  `radius` mm, the direction alternating from one centre to the next.
  (su, sv), where each pixel reads from before the warp, defaults to the
  pixel itself."""
  for n, (cu, cv) in enumerate(centres):
    du, dv = su - cu, sv - cv
    a = turn * (-1) ** n * np.exp(-(du * du + dv * dv) / radius ** 2)
    ca, sa = np.cos(a), np.sin(a)
    su, sv = cu + ca * du - sa * dv, cv + sa * du + ca * dv
  return su, sv


def swirled_tissue():
  """C4's sheet swirled about fourteen of its crowns, each turned up to 200
  degrees and fading over 11 mm, the turns alternating. Returns the
  swirled height blurred 2 mm (mm), its light from the upper right (0 to
  1), and the phase (cycles) of ruled lines 5 mm apart that ride up over
  the relief and swirl with it."""
  h = tissue()
  su, sv = swirl(crowns(h, 12, 14), 11.0, 3.5)
  hw = sample(h, su, sv)
  hb = sample(blur(h, 2.0), su, sv)
  a = math.radians(30)
  across = (su * math.cos(a) + sv * math.sin(a) - 1.5 * hb) / 5.0
  return hb, blur(tissue_tone(hw), 0.5), across


def c6_swirl_moire():
  """C5's contour bands on C4's sheet swirled about fourteen of its crowns,
  so the bands twist into whorls. Light still from the upper right on the
  swirled folds. A second family, ruled lines 5 mm apart riding up over
  the relief and swirled with it, swells the contour bands to near solid
  and pinches them to 1.0 mm where it crosses them, so the two pitches
  beat in moire across the mid tones."""
  hb, t, across = swirled_tissue()
  dh = 0.1
  return contour_bands(hb / dh, t, 3.2, 4, lo=0.15, hi=0.88, beat=(across, 0.4))


# --- C7: spiralling, dripping tissue -------------------------------------------

def drips(count, seed):
  """How far down, mm, each pixel reads the sheet from: `count` tongues,
  each 5 to 10 mm across, that pull the sheet down 5 to 14 mm over a
  stretch twice as long, then ease back over twice that again."""
  rng = np.random.default_rng(seed)
  d = np.zeros(U.shape)
  for _ in range(count):
    cu, cv = rng.uniform(0, W / S), rng.uniform(-20, H / S)
    sigma, amp = rng.uniform(2.0, 4.0), rng.uniform(5.0, 14.0)
    run = 2 * amp
    x = (V - cv) / run
    rise = np.clip(x, 0, 1)
    fall = np.clip((x - 1) / 2, 0, 1)
    p = rise * rise * (3 - 2 * rise) - fall * fall * (3 - 2 * fall)
    d += amp * np.exp(-0.5 * ((U - cu) / sigma) ** 2) * p
  return d


def wound(phase, centres, turns, rise, reach):
  """`phase` raised about each centre by a cone `rise` cycles high and
  `reach` mm in radius, so rings crowd toward the centre, plus `turns`
  whole cycles about the centre, the sign alternating, so the rings wind
  into spirals. Returns the phase and the cones' shape, 1 at a centre
  falling to 0 at the foot."""
  out, cone = phase, np.zeros(U.shape)
  for n, (cu, cv) in enumerate(centres):
    c = np.clip(1 - np.hypot(U - cu, V - cv) / reach, 0, None) ** 2
    out = out + rise * c + (-1) ** n * turns * np.arctan2(V - cv, U - cu) / (2 * math.pi)
    cone += c
  return out, np.minimum(cone, 1)


def clear_of_holes(points, radius):
  """The points (u, v) mm whose disc of `radius` mm lies wholly in the
  inlay field, clear of the edge, the holes and the OLED window."""
  keep = []
  for u, v in points:
    i0, i1 = int((u - radius) * S), int((u + radius) * S) + 1
    j0, j1 = int((v - radius) * S), int((v + radius) * S) + 1
    if i0 < 0 or j0 < 0 or i1 > W or j1 > H:
      continue
    disc = (U[j0:j1, i0:i1] - u) ** 2 + (V[j0:j1, i0:i1] - v) ** 2 <= radius ** 2
    if ALLOWED[j0:j1, i0:i1][disc].all():
      keep.append((u, v))
  return keep


def c7_spiral_drip():
  """C6's swirled sheet and moire at full tilt. Forty-five drips, 5 to 10
  mm across, pull bands and light down 5 to 14 mm across the facets like
  melting wax. On the nine highest crowns clear of the holes, a cone
  crowds the contour bands toward the crown and winds them into spirals,
  the light there pulled toward mid tone so every arm shows; elsewhere the
  pitch sweeps from 2.2 mm on high ground to 3.6 mm in the hollows. Light
  from the upper right, dripped with the bands: the bands swell from 1.0
  mm in shadow to solid on the crowns."""
  hb, t, across = swirled_tissue()
  height = equalize(hb, ALLOWED)
  dv = V - drips(45, 171)
  hb, t, across, height = (sample(f, U, dv) for f in (hb, t, across, height))
  levels = 4
  turns = 2 ** (levels - 1)
  centres = clear_of_holes(crowns(hb, 14, 60), 4.0)[:9]
  phase, cone = wound(hb / 0.1, centres, turns, 20.0, 14.0)
  t = 0.5 + (t - 0.5) * (1 - 0.7 * np.sqrt(cone))
  pitch = 3.6 - 1.4 * np.maximum(height, cone)
  return contour_bands(phase, t, pitch, levels, lo=0.15, hi=0.88,
                       beat=(across, 0.4 * (1 - np.sqrt(cone))), quantum=turns)


# --- C8: tissue in flat-sided shards -------------------------------------------

def poisson_disc(gap, seed, margin):
  """Points (n, 2) mm over the face and `margin` mm around it, no two closer
  than `gap` mm, and no spot left more than 2 x `gap` from a point."""
  rng = np.random.default_rng(seed)
  u0, v0, u1, v1 = -margin, -margin, W / S + margin, H / S + margin
  cell = gap / math.sqrt(2)
  nu, nv = int((u1 - u0) / cell) + 1, int((v1 - v0) / cell) + 1
  grid = np.full((nv, nu), -1)
  pts = np.empty((nu * nv, 2))
  n, active = 0, []

  def clear(p):
    if not (u0 <= p[0] < u1 and v0 <= p[1] < v1):
      return False
    i, j = int((p[0] - u0) / cell), int((p[1] - v0) / cell)
    near = grid[max(0, j - 2):j + 3, max(0, i - 2):i + 3]
    near = near[near >= 0]
    return not near.size or bool((((pts[near] - p) ** 2).sum(1) >= gap * gap).all())

  def place(p):
    nonlocal n
    pts[n] = p
    grid[int((p[1] - v0) / cell), int((p[0] - u0) / cell)] = n
    active.append(n)
    n += 1

  place(rng.uniform([u0, v0], [u1, v1]))
  while active:
    slot = rng.integers(len(active))
    a = active[slot]
    rad = gap * (1 + rng.random(30))
    ang = 2 * math.pi * rng.random(30)
    for p in pts[a] + np.column_stack([rad * np.cos(ang), rad * np.sin(ang)]):
      if clear(p):
        place(p)
        break
    else:
      active[slot] = active[-1]
      active.pop()
  return pts[:n]


def nearest(pts, reach):
  """For each pixel, the nearest point's index and the squared distance to
  it, mm^2. Every pixel lies within `reach` mm of a point."""
  best = np.full(U.shape, np.inf)
  idx = np.zeros(U.shape, np.int64)
  for k, (pu, pv) in enumerate(pts):
    i0, i1 = max(0, int((pu - reach) * S)), min(W, int((pu + reach) * S) + 1)
    j0, j1 = max(0, int((pv - reach) * S)), min(H, int((pv + reach) * S) + 1)
    if i0 >= i1 or j0 >= j1:
      continue
    d2 = (U[j0:j1, i0:i1] - pu) ** 2 + (V[j0:j1, i0:i1] - pv) ** 2
    b, c = best[j0:j1, i0:i1], idx[j0:j1, i0:i1]
    closer = d2 < b
    b[closer] = d2[closer]
    c[closer] = k
  assert np.isfinite(best).all()
  return idx, best


def neighbours(pts):
  """Each point's Voronoi neighbours, one row per point, padded with -1."""
  tris = delaunay(pts)
  edges = np.unique(np.sort(tris[:, [0, 1, 1, 2, 2, 0]].reshape(-1, 2), 1), axis=0)
  deg = np.bincount(edges.ravel(), minlength=len(pts))
  nbr = np.full((len(pts), deg.max()), -1)
  fill = np.zeros(len(pts), np.int64)
  for a, b in edges:
    nbr[a, fill[a]], nbr[b, fill[b]] = b, a
    fill[a] += 1
    fill[b] += 1
  return nbr


# Shard limits, mm. Between two shard cells each side of the dark channel
# takes CHANNEL; beside a chip cell the shard takes the whole channel and
# the chip cell keeps a light strip CHIP_EDGE wide. Between two chips each
# side of the light strip takes CHIP_GAP. SHARD_MIN and CHIP_MIN are the
# least distance from a cell's point to each side of its shard or chip.

CHANNEL = 0.38
CHIP_GAP = 0.45
CHIP_EDGE = 0.9
SHARD_MIN = 0.75
CHIP_MIN = 0.65


def shards(t, gap, seed, lo=0.06, hi=0.94, mid=0.5, mix=0.05):
  """A mosaic of flat-sided cells, the Voronoi cells of points no closer than
  `gap` mm. Each cell takes the mean tone over it, and its light share k
  follows that tone from bare dark under `lo` to solid light over `hi`.
  Below k = `mid`, give or take `mix` at random, a cell holds a light shard,
  the cell scaled about its point to area k and held off its sides by the
  dark channel. Above, the cell turns light and holds a dark chip, the
  cell scaled to area 1 - k and held off its sides by the light strip.
  Under the smallest size, a cell holds its shard or chip at that size by
  chance in proportion to k."""
  pts = poisson_disc(gap, seed, 3 * gap)
  nbr = neighbours(pts)
  cid, d2 = nearest(pts, 2.5 * gap)
  n = len(pts)
  valid = nbr >= 0
  j = np.where(valid, nbr, 0)
  half = np.where(valid, np.linalg.norm(pts[j] - pts[:, None], axis=2) / 2, 1.0)
  reach = np.where(valid, half, np.inf).min(1)

  inside = ALLOWED.ravel()
  cells = cid.ravel()[inside]
  mean = np.bincount(cells, t.ravel()[inside], n) / np.maximum(np.bincount(cells, None, n), 1)
  k = np.clip((mean - lo) / (hi - lo), 0, 1)

  rng = np.random.default_rng(seed + 1)
  flip, keep = rng.random(n), rng.random(n)
  chip = k > mid + mix * (2 * flip - 1)

  s_min = SHARD_MIN / reach
  s = np.sqrt(k)
  lit = ~chip & ((s >= s_min) | (keep * s_min ** 2 < k))
  s = np.maximum(s, s_min)
  c_min = CHIP_MIN / reach
  c = np.sqrt(1 - k)
  dot = chip & ((c >= c_min) | (keep * c_min ** 2 < 1 - k))
  c = np.maximum(c, c_min)

  # Offsets of each cell's sides toward its point: the shard or chip lies
  # where every side is further than its offset.

  near_chip = chip[j]
  off_shard = np.maximum((1 - s)[:, None] * half, np.where(near_chip, 2 * CHANNEL, CHANNEL))
  off_chip = np.maximum((1 - c)[:, None] * half, np.where(near_chip, CHIP_GAP, CHIP_EDGE))
  dot &= np.where(valid, half - off_chip, np.inf).min(1) >= CHIP_MIN
  off = np.where(valid, np.where(chip[:, None], off_chip, off_shard), -np.inf)

  m = np.full(U.shape, np.inf)
  for slot in range(nbr.shape[1]):
    q = j[cid, slot]
    e = ((U - pts[q, 0]) ** 2 + (V - pts[q, 1]) ** 2 - d2) / (4 * half[cid, slot])
    m = np.minimum(m, np.where(valid[cid, slot], e - off[cid, slot], np.inf))
  return np.where(chip[cid], ~dot[cid] | (m <= 0), lit[cid] & (m > 0))


def c8_shards():
  """C4's tissue, its light from the upper right, drawn in flat-sided shards:
  Voronoi cells about 3.7 mm across, their points at least 2.9 mm apart. In
  shadow each cell holds a light polygon, the cell scaled down, that grows
  with the light until straight dark channels 0.76 mm wide part it from its
  neighbours; past 58 % light the ground turns light and each cell holds a
  dark polygon chip that shrinks away on the brightest crowns."""
  h = tissue()
  return shards(tissue_tone(h), 2.9, 181, lo=0.08, hi=0.92, mid=0.58)


# --- C9: tissue shattered into splinters ---------------------------------------

def fold_flow(h, rmin, rmax, radius):
  """Direction (radians) and stretch of the splinters at each pixel. They run
  along the level lines of the sheet h averaged over `radius` mm, stretched
  from `rmin` where the folds turn every way up to `rmax` where they all run
  one way."""
  gv, gu = np.gradient(blur(h, 3.0), 1 / S)
  j11, j22, j12 = (blur(a, radius) for a in (gu * gu, gv * gv, gu * gv))
  across = 0.5 * np.arctan2(2 * j12, j11 - j22)
  order = np.hypot(j11 - j22, 2 * j12) / np.maximum(j11 + j22, 1e-12)
  return across + math.pi / 2, rmin + (rmax - rmin) * order


def stretched_seeds(gap, theta, ratio, seed, margin):
  """Points (n, 2) mm over the face and `margin` mm around it, spread like a
  Poisson disc in a stretched measure: along the direction `theta` distances
  count 1 / `ratio`, so neighbours sit at least `gap` mm apart across and
  `gap` x `ratio` along (all three are fields on the face grid). Returns the
  points and each one's gap, ratio and direction."""
  rng = np.random.default_rng(seed)
  u0, v0, u1, v1 = -margin, -margin, W / S + margin, H / S + margin
  cell = gap.min() / math.sqrt(2)
  nu, nv = int((u1 - u0) / cell) + 1, int((v1 - v0) / cell) + 1
  grid = np.full((nv, nu), -1)
  pts = np.empty((nu * nv, 2))
  pg, pr, pt = np.empty(nu * nv), np.empty(nu * nv), np.empty(nu * nv)
  n, active = 0, []

  def field(p):
    i = np.clip((p[..., 0] * S).astype(np.int64), 0, W - 1)
    j = np.clip((p[..., 1] * S).astype(np.int64), 0, H - 1)
    return gap[j, i], ratio[j, i], theta[j, i]

  def place(p):
    nonlocal n
    pts[n] = p
    pg[n], pr[n], pt[n] = (float(x) for x in field(p))
    grid[int((p[1] - v0) / cell), int((p[0] - u0) / cell)] = n
    active.append(n)
    n += 1

  place(rng.uniform([u0, v0], [u1, v1]))
  while active:
    slot = rng.integers(len(active))
    a = active[slot]
    g, r, th = pg[a], pr[a], pt[a]
    rad = g * (1 + rng.random(30))
    ang = 2 * math.pi * rng.random(30)
    x, y = rad * np.cos(ang) * r, rad * np.sin(ang)
    cand = pts[a] + np.column_stack([x * math.cos(th) - y * math.sin(th),
                                     x * math.sin(th) + y * math.cos(th)])
    i, j = int((pts[a, 0] - u0) / cell), int((pts[a, 1] - v0) / cell)
    m = int(3.4 * g * r / cell) + 1
    near = grid[max(0, j - m):j + m + 1, max(0, i - m):i + m + 1]
    near = near[near >= 0]
    cg, cr, ct = field(cand)
    du = pts[near, 0][None, :] - cand[:, :1]
    dv = pts[near, 1][None, :] - cand[:, 1:]
    c, s = np.cos(ct)[:, None], np.sin(ct)[:, None]
    d2 = ((du * c + dv * s) / cr[:, None]) ** 2 + (dv * c - du * s) ** 2
    lim = (cg[:, None] + pg[near][None, :]) / 2
    ok = ((cand[:, 0] >= u0) & (cand[:, 0] < u1) & (cand[:, 1] >= v0) & (cand[:, 1] < v1)
          & (d2 >= lim * lim).all(1))
    if ok.any():
      place(cand[np.argmax(ok)])
    else:
      active[slot] = active[-1]
      active.pop()
  return pts[:n].copy(), pg[:n].copy(), pr[:n].copy(), pt[:n].copy()


def stretched_nearest(pts, reach, theta, ratio):
  """For each pixel, the index of the nearest point in the pixel's own
  stretched measure, as in stretched_seeds. Point k reaches reach[k] mm."""
  ct, st, ir = np.cos(theta), np.sin(theta), 1 / ratio
  best = np.full(U.shape, np.inf)
  idx = np.full(U.shape, -1, np.int64)
  for k, (pu, pv) in enumerate(pts):
    i0, i1 = max(0, int((pu - reach[k]) * S)), min(W, int((pu + reach[k]) * S) + 1)
    j0, j1 = max(0, int((pv - reach[k]) * S)), min(H, int((pv + reach[k]) * S) + 1)
    if i0 >= i1 or j0 >= j1:
      continue
    win = np.s_[j0:j1, i0:i1]
    du, dv = U[win] - pu, V[win] - pv
    along = (du * ct[win] + dv * st[win]) * ir[win]
    across = dv * ct[win] - du * st[win]
    d2 = along * along + across * across
    b, c = best[win], idx[win]
    closer = d2 < b
    b[closer] = d2[closer]
    c[closer] = k
  assert (idx >= 0).all()
  return idx


def cell_neighbours(cid, n):
  """Each cell's neighbours, read off the pixel map of cell indices, one row
  per cell, padded with -1."""
  pairs = []
  for a, b in ((cid[:, 1:], cid[:, :-1]), (cid[1:], cid[:-1]),
               (cid[1:, 1:], cid[:-1, :-1]), (cid[1:, :-1], cid[:-1, 1:])):
    differ = a != b
    pairs.append(np.stack([a[differ], b[differ]], 1))
  edges = np.unique(np.sort(np.concatenate(pairs), 1), axis=0)
  deg = np.bincount(edges.ravel(), minlength=n)
  nbr = np.full((n, deg.max()), -1)
  fill = np.zeros(n, np.int64)
  for a, b in edges:
    nbr[a, fill[a]], nbr[b, fill[b]] = b, a
    fill[a] += 1
    fill[b] += 1
  return nbr


def edge_lines(pts, ratio, theta, nb):
  """The straight edge between each point and each of its neighbours nb
  (n, slots): the pair's bisector in their shared stretch. Returns the
  edge's unit normal, pointing into the point's cell, and its midpoint, as
  four (n, slots) arrays nx, ny, mx, my."""
  phi = 0.5 * np.arctan2(np.sin(2 * theta)[:, None] + np.sin(2 * theta)[nb],
                         np.cos(2 * theta)[:, None] + np.cos(2 * theta)[nb])
  r2 = ((ratio[:, None] + ratio[nb]) / 2) ** 2
  wu, wv = pts[:, None, 0] - pts[nb, 0], pts[:, None, 1] - pts[nb, 1]
  c, s = np.cos(phi), np.sin(phi)
  along, across = (wu * c + wv * s) / r2, wv * c - wu * s
  mu, mv = along * c - across * s, along * s + across * c
  norm = np.maximum(np.hypot(mu, mv), 1e-12)
  return (mu / norm, mv / norm,
          (pts[:, None, 0] + pts[nb, 0]) / 2, (pts[:, None, 1] + pts[nb, 1]) / 2)


def bent_edges(nb, nx, ny, seed):
  """Each straight edge from edge_lines, normal (nx, ny), bent 24 to 44
  degrees at its midpoint into a shallow V that is the same seen from both
  cells, so no edge runs straight across its whole length. Returns, per
  (cell, slot), the normals of the V's two arms, both toward its open side,
  and which way it opens, +1 into the cell."""
  me = np.arange(nb.shape[0])[:, None]
  key = np.minimum(me, nb) * 1000003 + np.maximum(me, nb)
  half = np.radians(10 + 10 * hash01(key, seed + 200))
  opens = np.where(me < nb, 1, -1) * np.where(hash01(key, seed + 201) < 0.5, 1, -1)
  ox, oy = opens * nx, opens * ny
  c, s = np.cos(half), np.sin(half)
  return c * ox + s * ny, c * oy - s * nx, c * ox - s * ny, c * oy + s * nx, opens


def bent_distance(arms, mx, my, cid, k, u, v):
  """Distance, mm, from (u, v) to slot k's bent edge of cell cid, positive
  inside the cell; (mx, my) are the edges' midpoints."""
  a1x, a1y, a2x, a2y, opens = (a[cid, k] for a in arms)
  du, dv = u - mx[cid, k], v - my[cid, k]
  return opens * np.minimum(a1x * du + a1y * dv, a2x * du + a2y * dv)


# Torn shard edges, mm: a V notch in some intervals along each edge, and a
# sawtooth on a share of the edges.

NOTCH_STEP = (3.0, 6.0)
NOTCH_RATE = 0.25
NOTCH_MOUTH = (1.6, 2.6)
NOTCH_DEPTH = (1.2, 2.4)
SAW_SHARE = 0.15
SAW_AMP = (1.0, 1.4)
SAW_STEP = (2.4, 3.4)


def edge_profile(s, key, saw_amp=0.0, saw_step=1.0):
  """How far, mm, the shard edge keyed `key` retreats at position s mm along
  it: a V notch in a NOTCH_RATE share of its intervals, plus a sawtooth
  where `saw_amp` is not 0. Every piece is straight."""
  step = NOTCH_STEP[0] + (NOTCH_STEP[1] - NOTCH_STEP[0]) * hash01(key, 1)
  x = s / step
  i = np.floor(x).astype(np.int64)
  f = x - i
  centre = 0.2 + 0.6 * hash01(key, i, 12)
  mouth = NOTCH_MOUTH[0] + (NOTCH_MOUTH[1] - NOTCH_MOUTH[0]) * hash01(key, i, 13)
  depth = NOTCH_DEPTH[0] + (NOTCH_DEPTH[1] - NOTCH_DEPTH[0]) * hash01(key, i, 14)
  notch = depth * np.clip(1 - np.abs(f - centre) * step / (mouth / 2), 0, None)
  y = s / saw_step
  return np.where(hash01(key, i, 11) < NOTCH_RATE, notch, 0.0) + saw_amp * (y - np.floor(y))


def fill_to_share(dist, inv, share, pix_share, min_half, taper):
  """Where dist (mm, -inf where never allowed) clears a threshold set per
  group (inv, each pixel's group) so the group's share is `share`, as far
  as dist allows. Within a group the threshold moves with `pix_share`, each
  pixel's own share, `taper` times as far as would change the group's share
  by as much, so shapes narrow toward lower share. A group whose share
  would leave it thinner than 2 x `min_half` mm stays empty. Returns the
  mask, which groups stayed empty with a share above 0, and the share each
  group was given."""
  fd = dist.ravel()
  ng = len(share)
  cnt = np.bincount(inv, minlength=ng)
  start = np.concatenate([[0], np.cumsum(cnt)[:-1]])
  ranked = fd[np.lexsort((-fd, inv))]
  top = ranked[start]
  room = np.bincount(inv, fd > -np.inf, ng) / cnt
  k = np.minimum(share, room)
  keep = np.floor(k * cnt).astype(np.int64)
  tau = np.where(keep > 0, ranked[start + np.maximum(keep - 1, 0)], np.inf)
  empty = (tau > top - min_half) | (k <= 0)

  # Share at an inset tau runs about as room x (1 - tau / top)^2.

  a = np.maximum(room, 1e-9)
  depth = np.where(empty, 0.0, taper * top)
  base = np.where(empty, np.inf, tau + depth * np.sqrt(k / a))
  tau_px = base[inv] - depth[inv] * np.sqrt(np.clip(pix_share.ravel(), 0, None) / a[inv])
  return (fd >= tau_px).reshape(dist.shape), empty & (k > 0), k


def splinters(groups, inv, share, empty, chip, ratio, theta, keep, seed):
  """Each group listed in `empty`, too small to hold an inset shard or chip,
  holds by chance in proportion to its share a splinter instead: a
  four-sided needle along its cell's stretch, pointed at both ends, light
  1.4 to 2.0 mm wide in a shard cell and dark 1.1 to 1.6 mm wide in a chip
  cell, at least 4 mm long, kept where `keep` >= 0. `groups` holds each
  group's id, cell x 4 + piece."""
  ng = len(groups)
  cnt = np.bincount(inv, minlength=ng)
  cu = np.bincount(inv, U.ravel(), ng) / cnt
  cv = np.bincount(inv, V.ravel(), ng) / cnt
  cell = groups // 4
  area = cnt / S ** 2
  dark = chip[cell]
  w0, w1 = np.where(dark, 1.1, 1.4), np.where(dark, 1.6, 2.0)
  w = w0 + (w1 - w0) * hash01(groups, seed + 101)
  length = np.maximum(np.sqrt(area * ratio[cell]), np.maximum(2 * w, 4.0))
  ang = theta[cell] + math.radians(15) * (2 * hash01(groups, seed + 102) - 1)
  at = 0.25 + 0.5 * hash01(groups, seed + 103)

  # The print limits shave about 0.6 mm off the needle's width along its length.

  odds = share * area / (length * w / 2 * (1 - 0.6 / w) ** 2)
  on = empty & (hash01(groups, seed + 104) < odds)
  g = inv.reshape(U.shape)
  du, dv = U - cu[g], V - cv[g]
  ca, sa = np.cos(ang)[g], np.sin(ang)[g]
  x = du * ca + dv * sa + length[g] / 2
  y = np.abs(dv * ca - du * sa)
  wide = at[g] * length[g]
  half = np.where(x < wide, x / wide, (length[g] - x) / (length[g] - wide)) * w[g] / 2
  return on[g] & (x >= 0) & (x <= length[g]) & (y <= half) & (keep >= 0)


# Cracks, mm, and degrees for the turns: C9's chains, then C9a's shorter and
# sharper ones, which never swap their turn at two joints running.

C9_CRACKS = dict(count=110, length=(8.0, 28.0), wide=(0.9, 1.4), step=(2.5, 4.5),
                 kink=(15.0, 50.0), branches=(0, 4), branch_len=(3.0, 10.0), steady=False)
C9A_CRACKS = dict(count=190, length=(6.0, 12.0), wide=(0.9, 1.2), step=(2.0, 4.0),
                  kink=(25.0, 40.0), branches=(1, 4), branch_len=(3.0, 7.0), steady=True)
CRACK_TIP = 0.55
BRANCH_ANGLE = (30.0, 75.0)


def tapered_segments(segs):
  """Pixels within half the width of any segment (p, q, width at p, width
  at q), the width running straight from end to end."""
  dark = np.zeros(U.shape, bool)
  for p, q, w0, w1 in segs:
    r = max(w0, w1) / 2 + 0.1
    i0, i1 = max(0, int((min(p[0], q[0]) - r) * S)), min(W, int((max(p[0], q[0]) + r) * S) + 1)
    j0, j1 = max(0, int((min(p[1], q[1]) - r) * S)), min(H, int((max(p[1], q[1]) + r) * S) + 1)
    if i0 >= i1 or j0 >= j1:
      continue
    du, dv = q - p
    pu, pv = U[j0:j1, i0:i1] - p[0], V[j0:j1, i0:i1] - p[1]
    f = np.clip((pu * du + pv * dv) / max(du * du + dv * dv, 1e-12), 0, 1)
    dark[j0:j1, i0:i1] |= np.hypot(pu - f * du, pv - f * dv) < (w0 + (w1 - w0) * f) / 2
  return dark


def cracks(seed, style):
  """Dark fracture lines, `style` one of C9_CRACKS and C9A_CRACKS: `count`
  chains `length` long of segments `step` long, turning by `kink` at every
  joint, either way at random, so no straight stretch runs past one segment
  and the chain wanders. Each chain is widest, `wide`, somewhere along its
  length and tapers to CRACK_TIP at both ends. Up to three branches leave
  each chain at BRANCH_ANGLE, `branch_len` long, kink the same way and taper
  to CRACK_TIP at their tips."""
  rng = np.random.default_rng(seed)
  segs = []

  # A steady chain never swaps its turning direction at two joints running,
  # since a steady zigzag averages out into a straight line.

  def run(p, ang, length, width):
    s = 0.0
    if style['steady']:
      turn, swapped = rng.choice([-1, 1]), False
    while s < length:
      step = min(length - s, rng.uniform(*style['step']))
      q = p + step * np.array([math.cos(ang), math.sin(ang)])
      segs.append((p, q, width(s), width(s + step)))
      p, s = q, s + step
      if style['steady']:
        swapped = not swapped and rng.random() < 0.5
        turn = -turn if swapped else turn
        ang += math.radians(rng.uniform(*style['kink'])) * turn
      else:
        ang += math.radians(rng.uniform(*style['kink'])) * rng.choice([-1, 1])

  for _ in range(style['count']):
    p = rng.uniform([-10, -10], [W / S + 10, H / S + 10])
    ang = rng.uniform(0, 2 * math.pi)
    length = rng.uniform(*style['length'])
    wide = rng.uniform(*style['wide'])
    peak = rng.uniform(0.2, 0.8) * length

    def width(s, length=length, wide=wide, peak=peak):
      x = s / peak if s < peak else (length - s) / (length - peak)
      return CRACK_TIP + (wide - CRACK_TIP) * min(max(x, 0), 1) ** 0.7

    first = len(segs)
    run(p, ang, length, width)
    trunk = segs[first:]
    for _ in range(rng.integers(*style['branches'])):
      a, b, w0, w1 = trunk[rng.integers(len(trunk))]
      f = rng.random()
      w = (w0 + f * (w1 - w0)) * 0.9
      if w < CRACK_TIP + 0.2:
        continue
      turn = math.radians(rng.uniform(*BRANCH_ANGLE)) * rng.choice([-1, 1])
      blen = rng.uniform(*style['branch_len'])
      run(a + f * (b - a), math.atan2(b[1] - a[1], b[0] - a[0]) + turn, blen,
          lambda s, w=w, blen=blen: w + (CRACK_TIP - w) * s / blen)
  return tapered_segments(segs)


def needle(cu, cv, ang, length, width, at, pad):
  """A four-sided needle centred on (cu, cv) mm: tips `length` mm apart along
  `ang`, widest (`width` mm) at fraction `at` from the back tip. Returns its
  window on the face grid, grown by `pad` px, and its mask there."""
  r = length / 2 + width
  i0, i1 = max(0, int((cu - r) * S) - pad), min(W, int((cu + r) * S) + 1 + pad)
  j0, j1 = max(0, int((cv - r) * S) - pad), min(H, int((cv + r) * S) + 1 + pad)
  win = np.s_[j0:j1, i0:i1]
  du, dv = U[win] - cu, V[win] - cv
  ca, sa = math.cos(ang), math.sin(ang)
  x = du * ca + dv * sa + length / 2
  y = np.abs(dv * ca - du * sa)
  wide = at * length
  half = np.where(x < wide, x / wide, (length - x) / (length - wide)) * width / 2
  return win, (x >= 0) & (x <= length) & (y <= half)


def debris(light, t, theta, seed, tries=300):
  """`light` strewn with splinters: light needles 3.5 to 8 mm long and 1.3
  to 2.0 mm wide on the dark of the mid shadows, then dark needles 3 to 7
  mm by 1.1 to 1.7 mm on the light of the mid lights, each turned near the
  stretch `theta` and laid only where it keeps clear of the other colour."""
  rng = np.random.default_rng(seed)
  out = light.copy()
  for dark in (False, True):
    clear = disc(19 if dark else 15)
    for _ in range(tries):
      cu, cv = rng.uniform(0, W / S), rng.uniform(0, H / S)
      i, j = int(cu * S), int(cv * S)
      if not ALLOWED[j, i]:
        continue
      tt = t[j, i]
      if dark:
        odds = 0.7 * np.clip((tt - 0.5) / 0.15, 0, 1) * np.clip((0.9 - tt) / 0.12, 0, 1)
        length, width = rng.uniform(3.0, 7.0), rng.uniform(1.1, 1.7)
      else:
        odds = 0.7 * np.clip((tt - 0.06) / 0.12, 0, 1) * np.clip((0.62 - tt) / 0.3, 0, 1)
        length, width = rng.uniform(3.5, 8.0), rng.uniform(1.3, 2.0)
      if rng.random() > odds:
        continue
      ang = theta[j, i] + math.radians(rng.normal(0, 40))
      win, mark = needle(cu, cv, ang, length, width, rng.uniform(0.25, 0.6), 12)
      if not mark.any():
        continue
      here, field = out[win], ALLOWED[win]
      if dark:
        if (dilate(mark, clear) & ~here & field).any():
          continue
        here &= ~mark
      else:
        if (dilate(mark, clear) & here).any() or not field[mark].all():
          continue
        here |= mark
  return out


def shattered(t, h, seed, lo, hi, mid, bent, mix=0.05):
  """C9's grain over tone t (0 to 1) on the sheet h; see c9_shattered. With
  `bent`, C9a's: every cell edge and cut bends at its middle, and the cracks
  are C9A_CRACKS."""
  theta, ratio = fold_flow(h, 1.8, 3.4, 10.0)
  gap = 1.4 + 3.6 * np.clip(blur(t, 2.5), 0, 1)
  pts, gaps, ratios, dirs = stretched_seeds(gap, theta, ratio, seed, 12.0)
  cid = stretched_nearest(pts, 2.6 * gaps * ratios + 1.0, theta, ratio)
  n = len(pts)
  nbr = cell_neighbours(cid, n)
  slots = nbr.shape[1]
  valid = nbr >= 0
  nb = np.where(valid, nbr, np.arange(n)[:, None])
  nx, ny, mx, my = edge_lines(pts, ratios, dirs, nb)
  arms = bent_edges(nb, nx, ny, seed)

  def edge_distance(k, u, v):
    if bent:
      return bent_distance(arms, mx, my, cid, k, u, v)
    return nx[cid, k] * (u - mx[cid, k]) + ny[cid, k] * (v - my[cid, k])

  # Pixels move to the cell on their side of each bent edge.

  if bent:
    owner = cid.copy()
    for k in range(slots):
      over = valid[cid, k] & (edge_distance(k, U, V) < 0)
      owner[over] = nb[cid, k][over]
    cid = owner
  rng = np.random.default_rng(seed + 1)

  # Past `mid` light share, give or take `mix`, a cell turns light and holds
  # a dark chip.

  inside = ALLOWED.ravel()
  cells = cid.ravel()[inside]
  mean = np.bincount(cells, t.ravel()[inside], n) / np.maximum(np.bincount(cells, None, n), 1)
  chip = np.clip((mean - lo) / (hi - lo), 0, 1) > mid + mix * (2 * rng.random(n) - 1)

  # A quarter of the cells hold their shard knocked askew, turned 3 to 11
  # degrees about the cell's point and shoved up to 0.7 mm. Each pixel reads
  # its shard from (eu, ev).

  moved = rng.random(n) < 0.25
  turn = np.where(moved, np.radians(rng.uniform(3, 11, n)) * rng.choice([-1, 1], n), 0.0)
  shove_dir = rng.uniform(0, 2 * math.pi, n)
  shove = np.where(moved, rng.uniform(0.0, 0.7, n), 0.0)
  pu, pv = pts[cid, 0], pts[cid, 1]
  ca, sa = np.cos(turn)[cid], np.sin(turn)[cid]
  eu = pu + ca * (U - pu) + sa * (V - pv) - (shove * np.cos(shove_dir))[cid]
  ev = pv - sa * (U - pu) + ca * (V - pv) - (shove * np.sin(shove_dir))[cid]

  # Each side of a channel keeps at least 0.3 mm beside a shard cell; beside
  # a chip cell a shard keeps 0.6 mm and the chip cell a light strip 0.9 mm;
  # between chip cells each side of the light strip keeps 0.45 mm. Sides
  # widen at random, mostly a little, so channels run 0.6 to 2 mm.

  near_chip = chip[nb]
  least_shard = np.where(near_chip, 0.6, 0.3)
  least_chip = np.where(near_chip, 0.45, 0.9)
  side_shard = least_shard + 0.55 * rng.random((n, slots)) ** 4
  side_chip = least_chip + 0.5 * rng.random((n, slots)) ** 4
  saw_amp = np.where(rng.random((n, slots)) < SAW_SHARE, rng.uniform(*SAW_AMP, (n, slots)), 0.0)
  saw_step = rng.uniform(*SAW_STEP, (n, slots))

  # Inside each cell, mm: the distance in from the shard's and the chip's
  # outline, and from the least channel. A chip cell's light also tears
  # back from each edge it shares with a shard cell, so no plate keeps a
  # long straight rim.

  d_shard = np.full(U.shape, np.inf)
  d_chip = np.full(U.shape, np.inf)
  keep_shard = np.full(U.shape, np.inf)
  keep_chip = np.full(U.shape, np.inf)
  rim = np.full(U.shape, np.inf)
  for k in range(slots):
    live = valid[cid, k]
    ex, ey = nx[cid, k], ny[cid, k]
    here = edge_distance(k, U, V)
    read = edge_distance(k, eu, ev)
    torn = edge_profile(-ey * eu + ex * ev, cid * 64 + k + seed * 7919,
                        saw_amp[cid, k], saw_step[cid, k])
    d_shard = np.minimum(d_shard, np.where(live, read - side_shard[cid, k] - torn, np.inf))
    d_chip = np.minimum(d_chip, np.where(live, read - side_chip[cid, k], np.inf))
    keep_shard = np.minimum(keep_shard, np.where(live, here - least_shard[cid, k], np.inf))
    keep_chip = np.minimum(keep_chip, np.where(live, here - least_chip[cid, k], np.inf))
    rim = np.minimum(rim, np.where(live & ~near_chip[cid, k], here - torn, np.inf))

  # One or two cuts split a cell, more often the bigger it is, most at 4 to
  # 20 degrees to its stretch so they leave long wedges and slivers. When
  # `bent`, each cut bends 15 to 35 degrees where it passes near the cell's
  # point, so no cut runs straight across a whole cell. Chip cells take
  # fewer, and there a cut is a light strip through the chip.

  size = gaps * np.sqrt(ratios)
  odds = np.where(chip, 0.4, 1.0)
  cuts = ((rng.random(n) < odds * np.clip(0.15 + 0.12 * size, 0, 0.95)).astype(int)
          + (rng.random(n) < odds * np.clip(-0.2 + 0.08 * size, 0, 0.7)).astype(int))
  group = cid * 4
  for k in range(2):
    on = (cuts > k)[cid]
    ang = dirs + np.radians(rng.uniform(4, 20, n)) * rng.choice([-1, 1], n)
    ang = np.where(rng.random(n) < 0.15, rng.uniform(0, math.pi, n), ang)
    off = rng.uniform(-0.45, 0.45, n) * gaps
    qx, qy = pts[:, 0] - np.sin(ang) * off, pts[:, 1] + np.cos(ang) * off
    bend = np.radians(rng.uniform(15, 35, n)) * rng.choice([-1, 1], n) if bent else 0.0
    half_shard = 0.3 + 0.4 * rng.random(n) ** 2
    half_chip = 0.45 + 0.3 * rng.random(n) ** 2
    xu, xv = eu - qx[cid], ev - qy[cid]
    if bent:

      # The cut is two rays from (qx, qy), along ang + bend / 2 and back along
      # ang - bend / 2. Each pixel takes the nearer ray for its distance, its
      # side and its place along the cut.

      reach, sides, alongs = [], [], []
      for a, sign in ((ang + bend / 2, 1), (ang - bend / 2 + math.pi, -1)):
        du, dv = np.cos(a)[cid], np.sin(a)[cid]
        t = xu * du + xv * dv
        c = du * xv - dv * xu
        reach.append(np.where(t >= 0, np.abs(c), np.hypot(xu, xv)))
        sides.append(sign * c)
        alongs.append(sign * t)
      first = reach[0] <= reach[1]
      gap_to = np.minimum(reach[0], reach[1])
      side = np.where(first, sides[0], sides[1])
      along = np.where(first, alongs[0], alongs[1])
    else:
      cx, cy = -np.sin(ang)[cid], np.cos(ang)[cid]
      side = cx * xu + cy * xv
      gap_to = np.abs(side)
      along = -cy * eu + cx * ev
    torn = edge_profile(along, cid * 64 + 50 + k + seed * 7919)
    d_shard = np.where(on, np.minimum(d_shard, gap_to - half_shard[cid] - torn), d_shard)
    d_chip = np.where(on, np.minimum(d_chip, gap_to - half_chip[cid]), d_chip)
    group = group + np.where(on & (side > 0), 1 << k, 0)

  chip_px = chip[cid]
  keep = np.where(chip_px, keep_chip, keep_shard)
  dist = np.where(keep >= 0, np.where(chip_px, d_chip, d_shard), -np.inf)
  dropped = rng.random(n) < 0.06
  crack = cracks(seed + 3, C9A_CRACKS if bent else C9_CRACKS)

  # Each piece takes its share from its mean tone. A chip gives up the area a
  # crack already darkens; 6 % of cells and 16 % of cut pieces are missing.

  groups, inv = np.unique(group.ravel(), return_inverse=True)
  ng = len(groups)
  cell = groups // 4
  tone = (np.bincount(inv[inside], t.ravel()[inside], ng)
          / np.maximum(np.bincount(inv[inside], None, ng), 1))
  k = np.clip((tone - lo) / (hi - lo), 0, 1)
  cracked = np.bincount(inv, crack.ravel(), ng) / np.bincount(inv, None, ng)
  share = np.where(chip[cell], np.maximum(1 - k - cracked, 0), k)
  broken = (hash01(groups, seed + 5) < 0.16) & (cuts[cell] > 0)
  share = np.where(dropped[cell] | broken, 0.0, share)

  kp = np.clip((blur(t, 0.8) - lo) / (hi - lo), 0, 1)
  fill, empty, given = fill_to_share(dist, inv, share, np.where(chip_px, 1 - kp, kp), 0.6, 3.0)
  fill |= splinters(groups, inv, given, empty, chip, ratios, dirs, keep, seed)
  light = np.where(chip_px, ~fill & (rim >= 0), fill) & ~crack & ALLOWED
  return debris(light, t, theta, seed + 4)


def c9_shattered():
  """C4's tissue, its light from the upper right, shattered. Cells stretched
  along the folds grow from about 3 by 7 mm in shadow to 6 by 13 mm on the
  crowns, and straight cuts split most of them into wedges and slivers. In
  shadow the light shards narrow toward the dark, the smallest are needles
  1.4 to 2.0 mm wide, and torn channels 0.6 to 2 mm wide part them; some
  sit askew or are missing. Past 58 % light the ground turns light under
  dark chips and needles that vanish on the crowns, and the light plates
  tear back from their rims. A hundred and ten short cracks, 8 to 28 mm
  long and up to 1.4 mm wide, kink every 2.5 to 4.5 mm, branch and taper to
  points, and stray splinters lie in the mid tones."""
  h = tissue()
  return shattered(tissue_tone(h), h, 191, lo=0.06, hi=0.84, mid=0.58, bent=False)


def c9a_shattered():
  """C9 with every cell edge and cut bent at its middle, so no channel runs
  straight for more than about 8 mm, and its cracks shorter and sharper: a
  hundred and ninety chains 6 to 12 mm long of segments 2 to 4 mm that kink
  25 to 40 degrees at every joint, branch and taper to points."""
  h = tissue()
  return shattered(tissue_tone(h), h, 191, lo=0.06, hi=0.84, mid=0.58, bent=True)


# --- C10: balled sheet, dark and shattered -------------------------------------

def pointed_folds(count, length, reach, amp, seed, tip, ridge_share=0.5):
  """C3's folds as flat-sided tents: the same folds fold_lines() lays with
  these arguments, but each ends in a point `tip` times its reach beyond
  the fold (a range) instead of a round cap, so the sheet is flat planes
  meeting in straight creases. Returns the height h (mm), its slope gu, gv,
  the fold that rises or sinks most at each pixel (-1 for none), and each
  fold as (cu, cv, half, rho, a, th, k)."""
  rng = np.random.default_rng(seed)
  tips = np.random.default_rng(seed + 1)
  h, gu, gv, top = (np.zeros(U.shape) for _ in range(4))
  owner = np.full(U.shape, -1)
  folds = []
  for _ in range(count):
    cu, cv = rng.uniform(-10, W / S + 10), rng.uniform(-10, H / S + 10)
    half = rng.uniform(*length) / 2
    rho = rng.uniform(*reach)
    a = rng.uniform(*amp) * (1 if rng.random() < ridge_share else -1)
    th = rng.uniform(0, math.pi)
    k = tips.uniform(*tip)
    ca, sa = math.cos(th), math.sin(th)
    ext = half + k * rho
    i0, i1 = max(0, int((cu - ext) * S)), min(W, int((cu + ext) * S) + 1)
    j0, j1 = max(0, int((cv - ext) * S)), min(H, int((cv + ext) * S) + 1)
    if i0 >= i1 or j0 >= j1:
      continue
    du, dv = U[j0:j1, i0:i1] - cu, V[j0:j1, i0:i1] - cv
    along = du * ca + dv * sa
    across = dv * ca - du * sa
    over = np.maximum(np.abs(along) - half, 0)
    p = np.clip(1 - np.abs(across) / rho - over / (k * rho), 0, None)
    on = p > 0
    d_along = -a * np.sign(along) * (over > 0) * on / (k * rho)
    d_across = -a * np.sign(across) * on / rho
    h[j0:j1, i0:i1] += a * p
    gu[j0:j1, i0:i1] += d_along * ca - d_across * sa
    gv[j0:j1, i0:i1] += d_along * sa + d_across * ca
    win = abs(a) * p > top[j0:j1, i0:i1]
    top[j0:j1, i0:i1][win] = abs(a) * p[win]
    owner[j0:j1, i0:i1][win] = len(folds)
    folds.append((cu, cv, half, rho, a, th, k))
  return h, gu, gv, owner, folds


def fold_faces(owner, folds):
  """Per pixel, the flat face of its owning fold it lies on, as owner x 8
  plus the side across the fold and which end, if any; -1 where no fold."""
  face = np.where(owner >= 0, owner * 8, -1)
  for f, (cu, cv, half, rho, a, th, k) in enumerate(folds):
    ext = half + k * rho
    i0, i1 = max(0, int((cu - ext) * S)), min(W, int((cu + ext) * S) + 1)
    j0, j1 = max(0, int((cv - ext) * S)), min(H, int((cv + ext) * S) + 1)
    mask = owner[j0:j1, i0:i1] == f
    du, dv = U[j0:j1, i0:i1][mask] - cu, V[j0:j1, i0:i1][mask] - cv
    along = du * math.cos(th) + dv * math.sin(th)
    across = dv * math.cos(th) - du * math.sin(th)
    face[j0:j1, i0:i1][mask] += 4 * (across >= 0) + 2 * (along > half) + (along < -half)
  return face


def lit_side(fold, light):
  """+1 or -1: the side of a fold, across it, whose face turns to the light."""
  cu, cv, half, rho, a, th, k = fold
  return 1 if a * (-math.sin(th) * light[0] + math.cos(th) * light[1]) >= 0 else -1


def jagged(p0, p1, rng, run=2.6, wobble=(0.25, 0.7)):
  """A path from p0 to p1, mm, as vertices about `run` mm apart, each
  pushed `wobble` mm off the line to alternate sides, so the path kinks at
  every vertex and no straight run passes 4 mm."""
  (u0, v0), (u1, v1) = p0, p1
  span = math.hypot(u1 - u0, v1 - v0)
  n = max(1, math.ceil(span / run))
  nu, nv = (v0 - v1) / span, (u1 - u0) / span
  sign = rng.choice((-1, 1))
  pts = [p0]
  for m in range(1, n):
    t = (m + rng.uniform(-0.15, 0.15)) / n
    off = sign * rng.uniform(*wobble)
    sign = -sign
    pts.append((u0 + (u1 - u0) * t + nu * off, v0 + (v1 - v0) * t + nv * off))
  return pts + [p1]


def path_segments(pts, w0, w1):
  """A polyline as segments for strokes(), the width running from w0 at
  the start to w1 at the end."""
  n = len(pts) - 1
  return [(*pts[m], *pts[m + 1], w0 + (w1 - w0) * m / n, w0 + (w1 - w0) * (m + 1) / n)
          for m in range(n)]


def blade_breaks(folds, light, seed, spacing, chosen, lean=(30.0, 60.0), width=0.75):
  """Kinked cuts, as segments, that break the lit face of each `chosen`
  fold every `spacing` mm along it (a range), so no piece runs long: each
  crosses the face from crest to foot, leaning `lean` degrees off the
  fold, so each piece ends in a point."""
  rng = np.random.default_rng(seed)
  segs = []
  for fold, keep in zip(folds, chosen):
    if not keep:
      continue
    cu, cv, half, rho, a, th, k = fold
    side = lit_side(fold, light)
    s0 = -half - k * rho + rng.uniform(*spacing) / 2
    while s0 < half + k * rho:
      slope = rng.choice((-1, 1)) / math.tan(math.radians(rng.uniform(*lean)))
      ends = [(s0 - 0.3 * slope, -0.3 * side), (s0 + (rho + 0.3) * slope, (rho + 0.3) * side)]
      p0, p1 = [(cu + s * math.cos(th) - c * math.sin(th), cv + s * math.sin(th) + c * math.cos(th))
                for s, c in ends]
      segs += path_segments(jagged(p0, p1, rng), width, width)
      s0 += rng.uniform(*spacing)
  return segs


def crest_cracks(folds, light, seed, chance, length):
  """Short cracks that fan off a fold's crest into its lit face, as
  segments: on `chance` of the folds, two or three from each of one or two
  points on the crest, 35 to 80 degrees off the crest either way, `length`
  mm long, 1.0 mm tapering to 0.7 mm."""
  rng = np.random.default_rng(seed)
  segs = []
  for fold in folds:
    if rng.random() > chance:
      continue
    cu, cv, half, rho, a, th, k = fold
    side = lit_side(fold, light)
    for _ in range(rng.integers(1, 3)):
      s = rng.uniform(-half, half)
      pu, pv = cu + s * math.cos(th), cv + s * math.sin(th)
      for _ in range(rng.integers(2, 4)):
        psi = math.radians(rng.uniform(35, 80)) * side
        ang = th + (psi if rng.random() < 0.5 else math.pi - psi)
        size = rng.uniform(*length)
        segs.append((pu, pv, pu + size * math.cos(ang), pv + size * math.sin(ang), 1.0, 0.7))
  return segs


def impact_points(count, seed, gap=45.0, clear=6.0):
  """`count` points (u, v) mm at least `gap` mm apart, each with a disc
  `clear` mm in radius wholly in the inlay field."""
  rng = np.random.default_rng(seed)
  centres = []
  while len(centres) < count:
    p = tuple(rng.uniform((12, 12), (W / S - 12, H / S - 12)))
    if all(math.dist(p, c) > gap for c in centres) and clear_of_holes([p], clear):
      centres.append(p)
  return centres


def facet_exit(sheet, u, v, ang):
  """Distance, mm, from (u, v) heading `ang` radians to the edge of the
  sheet facet it lies in; infinite off the face."""
  i, j = int(u * S), int(v * S)
  if not (0 <= i < W and 0 <= j < H):
    return math.inf
  w = sheet.w[sheet.fid[j, i]]
  lam = w[0] * u + w[1] * v + w[2]
  rate = w[0] * math.cos(ang) + w[1] * math.sin(ang)
  ahead = [-l / r for l, r in zip(lam, rate) if r < -1e-12]
  return min(ahead) if ahead else math.inf


def impact_cracks(centres, seed, sheet, spokes, length, wander=22.0, ring=(7.0, 10.0), wide=1.2,
                  tip=0.7, skip=0.2):
  """Cracks like struck glass, as segments: from each centre `spokes`
  cracks run outward `length` mm. A crack runs straight only to the edge
  of the sheet facet it crosses, 1.2 to 3.6 mm, then kinks at least 12
  degrees, zigzagging within `wander` degrees of its outward line; `skip`
  of its segments are left out, so it reads as broken fragments, never a
  line. Cracks fork into twigs 2 to 7 mm long that set off 25 to 60
  degrees from the crack's last segment, and all taper from `wide` mm at
  the impact to `tip` mm at every end. One ring of kinked chords, `ring`
  mm out, runs between half the neighbouring cracks, each chord stopping
  0.7 mm short of both, so no two chords join."""
  rng = np.random.default_rng(seed)
  segs = []

  def crack(u, v, base, size, w_start, depth):
    path, left = [(u, v)], size
    dev = rng.uniform(-wander, wander) if depth == 0 else 0.0
    while left > 0.6:
      ang = base + math.radians(dev)
      step = min(left, float(np.clip(facet_exit(sheet, u, v, ang) + 0.05, 1.2, 3.6)))
      u1, v1 = u + step * math.cos(ang), v + step * math.sin(ang)
      wa = tip + (w_start - tip) * left / size
      wb = tip + (w_start - tip) * (left - step) / size
      if rng.random() >= skip:
        segs.append((u, v, u1, v1, wa, wb))
      left -= step
      if depth < 2 and left > 2 and rng.random() < 0.3:
        fork = ang + math.radians(rng.uniform(25, 60)) * rng.choice((-1, 1))
        crack(u1, v1, fork, min(left, rng.uniform(2, 7)), wb, depth + 1)
      u, v = u1, v1
      path.append((u, v))
      turn = rng.uniform(12, wander)
      dev = dev - turn if dev > 0 else dev + turn
    return path

  for cu, cv in centres:
    k = rng.integers(*spokes)
    paths = [crack(cu, cv, a, rng.uniform(*length), wide, 0)
             for a in (np.arange(k) + rng.uniform(-0.3, 0.3, k)) * 2 * math.pi / k + rng.uniform(0, 2 * math.pi)]
    r = rng.uniform(*ring)
    rim = [point_at(p, r * rng.uniform(0.85, 1.15)) for p in paths]
    for m in range(k):
      (ua, va), (ub, vb) = rim[m], rim[(m + 1) % k]
      span = math.hypot(ub - ua, vb - va)
      if rng.random() < 0.5 and span > 2.4:
        t = 0.7 / span
        p0 = (ua + (ub - ua) * t, va + (vb - va) * t)
        p1 = (ub - (ub - ua) * t, vb - (vb - va) * t)
        segs += path_segments(jagged(p0, p1, rng), 0.8, 0.8)
  return segs


def point_at(path, r):
  """The point `r` mm along a polyline [(u, v), ...], or its end."""
  for (ua, va), (ub, vb) in zip(path, path[1:]):
    step = math.hypot(ub - ua, vb - va)
    if r <= step:
      return ua + (ub - ua) * r / step, va + (vb - va) * r / step
    r -= step
  return path[-1]


def outward(pu, pv, centres, reach):
  """The heading, radians, from the nearest centre within `reach` mm out to
  (pu, pv), or None if none is that close."""
  near = [(math.dist((pu, pv), c), c) for c in centres]
  d, (cu, cv) = min(near) if near else (math.inf, (0, 0))
  return math.atan2(pv - cv, pu - cu) if d < reach else None


def read_mask(m, u, v):
  """Mask m read at (u, v) mm, nearest pixel, edges held."""
  return m[np.clip((v * S).astype(np.int64), 0, H - 1), np.clip((u * S).astype(np.int64), 0, W - 1)]


def strokes(segs, light=None, room=0.9):
  """Pixels within half the width of any segment (u0, v0, u1, v1, w0, w1),
  mm, the width running from w0 at the start to w1 at the end. Each start
  overlaps back a little to close the joint; each end is cut square. Given
  the `light` mask, a segment also takes any strip of light beside it
  narrower than `room` mm, counting the segments drawn before it."""
  out = np.zeros(U.shape, bool)
  if light is not None:
    light = light.copy()
  for u0, v0, u1, v1, wa, wb in segs:
    r = max(wa, wb) / 2 + (0.2 if light is None else room + 0.1)
    i0, i1 = max(0, int((min(u0, u1) - r) * S)), min(W, int((max(u0, u1) + r) * S) + 1)
    j0, j1 = max(0, int((min(v0, v1) - r) * S)), min(H, int((max(v0, v1) + r) * S) + 1)
    eu, ev = u1 - u0, v1 - v0
    n = math.hypot(eu, ev)
    if i0 >= i1 or j0 >= j1 or n < 1e-6:
      continue
    pu, pv = U[j0:j1, i0:i1] - u0, V[j0:j1, i0:i1] - v0
    f = (pu * eu + pv * ev) / (n * n)
    side = (pu * ev - pv * eu) / n
    d = np.abs(side)
    w = wa + np.clip(f, 0, 1) * (wb - wa)
    span = (f >= -0.3 * wa / n) & (f <= 1)
    hit = span & (d < w / 2)
    if light is not None:
      band = span & (d < w / 2 + room)
      for q in (1 / 3, 2 / 3, 1):
        reach = np.sign(side) * np.maximum(w / 2 + q * room - d, 0)
        hit |= band & ~read_mask(light, U[j0:j1, i0:i1] + reach * ev / n, V[j0:j1, i0:i1] - reach * eu / n)
      light[j0:j1, i0:i1] &= ~hit
    out[j0:j1, i0:i1] |= hit
  return out


def grow(m, r):
  """m dilated by a disc `r` px in radius."""
  out = m.copy()
  n = int(r)
  for dy in range(-n, n + 1):
    for dx in range(-n, n + 1):
      if (dy or dx) and dy * dy + dx * dx <= r * r:
        out |= shift(m, dy, dx)
  return out


def chips(near, room, seed, gap, tip, base, splay, inradius, centres=(), reach=0.0):
  """Small thorn-shaped triangles at points at least `gap` mm apart where
  `near` holds, six in ten of them: a tip `tip` mm out from the point, two
  base corners `base` mm back, splayed `splay` radians either side of
  straight back. Within `reach` mm of a centre the tip points away from it,
  give or take 20 degrees; elsewhere anywhere. A chip is kept only if its
  inradius reaches `inradius` mm and every pixel of it lies in `room`."""
  rng = np.random.default_rng(seed)
  out = np.zeros(U.shape, bool)
  for pu, pv in poisson_disc(gap, seed, 0):
    i, j = int(pu * S), int(pv * S)
    if not (0 <= i < W and 0 <= j < H and near[j, i]) or rng.random() > 0.6:
      continue
    a = rng.uniform(0, 2 * math.pi)
    away = outward(pu, pv, centres, reach)
    if away is not None:
      a = away + math.radians(rng.uniform(-20, 20))
    s = rng.uniform(*splay)
    corners = np.array([[math.cos(a), math.sin(a)], [math.cos(a + math.pi - s), math.sin(a + math.pi - s)],
                        [math.cos(a + math.pi + s), math.sin(a + math.pi + s)]])
    corners *= np.array([rng.uniform(*tip), rng.uniform(*base), rng.uniform(*base)])[:, None]
    corners += (pu, pv)
    sides = np.linalg.norm(corners - np.roll(corners, 1, 0), axis=1)
    e1, e2 = corners[1] - corners[0], corners[2] - corners[0]
    cross = e1[0] * e2[1] - e1[1] * e2[0]
    if abs(cross) / sides.sum() < inradius:
      continue
    i0, i1 = max(0, int(corners[:, 0].min() * S)), min(W, int(corners[:, 0].max() * S) + 2)
    j0, j1 = max(0, int(corners[:, 1].min() * S)), min(H, int(corners[:, 1].max() * S) + 2)
    uu, vv = U[j0:j1, i0:i1], V[j0:j1, i0:i1]
    inside = np.ones(uu.shape, bool)
    for m in range(3):
      (ua, va), (ub, vb) = corners[m], corners[(m + 1) % 3]
      inside &= math.copysign(1, cross) * ((ub - ua) * (vv - va) - (vb - va) * (uu - ua)) >= 0
    if inside.any() and room[j0:j1, i0:i1][inside].all():
      out[j0:j1, i0:i1] |= inside
  return out


def slashes(light, seed, gap, length, depth=1.2, centres=(), reach=0.0):
  """Short dark slashes, as segments: at half of a set of points at least
  `gap` mm apart that lie `depth` mm or more inside the light, a cut
  `length` mm long, 0.8 mm wide tapering to 0.7 mm. Within `reach` mm of a
  centre it points away from it, give or take 15 degrees; elsewhere
  anywhere."""
  rng = np.random.default_rng(seed)
  deep = ~grow(~light, depth * S)
  segs = []
  for pu, pv in poisson_disc(gap, seed, 0):
    i, j = int(pu * S), int(pv * S)
    if 0 <= i < W and 0 <= j < H and deep[j, i] and rng.random() < 0.5:
      a, size = rng.uniform(0, math.pi), rng.uniform(*length)
      away = outward(pu, pv, centres, reach)
      if away is not None:
        a = away + math.radians(rng.uniform(-15, 15))
      segs.append((pu - size / 2 * math.cos(a), pv - size / 2 * math.sin(a),
                   pu + size / 2 * math.cos(a), pv + size / 2 * math.sin(a), 0.8, 0.7))
  return segs


def c10_shattered():
  """C3's balled sheet gone dark and shattered. C3's folds end in points,
  so the sheet is flat planes meeting in straight creases, still tilted a
  little by C3's facets, and each fold face is lit or dark whole, like a
  woodcut. Light comes low from the left and only faces turned squarely to
  it show; a face more than half in the cast shadows, which reach 12 mm,
  goes dark. Kinked cuts every 7 to 12 mm break each lit face into blades
  and thorns. Five impacts shatter it like struck glass: around each the
  light crushes solid within 10 mm and fades back by 26 mm, and 14 to 20
  broken cracks, 1.0 mm wide tapering to 0.7 mm, kinking at every C3 facet
  and never straight past 3.6 mm, splinter it outward into shards round a
  dark hub. Thorn chips and dark slashes fly outward from each impact;
  short cracks fan off the crests. Every cut and crack takes any strip of
  light beside it narrower than 0.9 mm."""
  h, gu, gv, owner, folds = pointed_folds(900, (6.0, 20.0), (1.5, 4.0), (0.6, 1.4), 131, (1.3, 2.5))
  facets = Sheet(3.2, 0.25, 132)
  h, gu, gv = h + facets.h, gu + facets.gu, gv + facets.gv
  light = light_dir(210, 28)
  centres = impact_points(5, 1017)
  crush = np.zeros(U.shape)
  for cu, cv in centres:
    crush = np.maximum(crush, np.clip(1 - np.hypot(U - cu, V - cv) / 26.0, 0, 1))

  face = (fold_faces(owner, folds) + 1).ravel()
  n = face.max() + 1
  area = np.maximum(np.bincount(face, None, n), 1)
  glow = np.bincount(face, lit(gu, gv, light).ravel(), n) / area
  shade = np.bincount(face, cast_shadow(h, light_dir(210, 15), 12.0).ravel().astype(float), n) / area
  face = face.reshape(U.shape)
  shine = grow(tone(np.where(owner >= 0, glow[face], 0.0)) > 0.72 - 1.2 * crush, 3.5)
  out = shine & ~((shade[face] > 0.5) & (hash01(owner, 77) > 1.6 * crush))

  shown = np.bincount(face.ravel(), out.ravel(), n) / S ** 2
  lit_face = np.arange(len(folds)) * 8 + 4 * np.array([lit_side(f, light) > 0 for f in folds]) + 1
  calm = read_mask(crush, np.array([f[0] for f in folds]), np.array([f[1] for f in folds])) < 0.1
  out &= ~strokes(blade_breaks(folds, light, 1001, (7.0, 12.0), (shown[lit_face] >= 6.0) & calm), out)
  out &= ~strokes(impact_cracks(centres, 1006, facets, (14, 21), (10.0, 30.0), ring=(11.0, 15.0), wide=1.0), out)
  out &= ~strokes(crest_cracks(folds, light, 1007, 0.06, (1.5, 3.5)), out)
  near = (blur(out.astype(float), 1.5) > 0.04) | (crush > 0)
  out |= chips(near & ~out, ~grow(out, 7) & ALLOWED, 1008, 4.0, (2.4, 3.4), (1.2, 1.6), (0.5, 0.8), 0.55,
               centres, 26.0)
  spray = np.min([np.hypot(U - cu, V - cv) for cu, cv in centres], axis=0) < 32.0
  out |= chips(spray & ~out, ~grow(out, 7) & ALLOWED, 1010, 3.0, (2.6, 3.6), (1.1, 1.4), (0.45, 0.7), 0.5,
               centres, 32.0)
  out &= ~strokes(slashes(out, 1009, 3.5, (1.5, 3.0), centres=centres, reach=26.0), out)
  return out


# --- C12: tissue in circles ---------------------------------------------------

def invariant_radii():
  """Radii in mm, 0.47 up, whose stamped disc the 8 px opening leaves whole."""
  out = []
  for k in range(47, 160):
    dy, dx = disc_offsets(k / 10)
    m = np.zeros((60, 60), bool)
    m[dy + 30, dx + 30] = True
    if opening(m, OPEN_DISC).sum() == m.sum():
      out.append(k / 100)
  return np.array(out)


RADII = invariant_radii()


def fit(r):
  """Largest radius at or under r mm that survives the 8 px opening whole."""
  ok = RADII[RADII <= r + 1e-9]
  return ok[-1] if len(ok) else 0.0


def leaves_allowed(u, v, r):
  """True when the disc (u, v, r mm) leaves ALLOWED."""
  dy, dx = disc_offsets(r * S)
  j, i = dy + round(v * S), dx + round(u * S)
  ok = (j >= 0) & (j < H) & (i >= 0) & (i < W)
  return (not ok.all()) or (not ALLOWED[j[ok], i[ok]].all())


def c12_circle_list():
  """(u, v, r) circles in mm on the face grid for C12."""
  h = blur(Sheet(12.0, 4.5, 141).h, 1.5)
  gv, gu = np.gradient(h, 1 / S)
  t = blur(tone(lit(gu, gv, light_dir(315, 35))), 0.3)
  rng = random.Random(145)
  raw = scatter(144, lambda r: r.uniform(0.6, 0.75), 1.2, 0.75, k=80)
  sites = np.array([(round(u * S) / S, round(v * S) / S) for u, v, _ in raw])
  nn = cKDTree(sites).query(sites, k=2)[0][:, 1]
  GAP, EDGE = 0.65, 0.15
  ts = lambda u, v: t[min(H - 1, int(v * S)), min(W - 1, int(u * S))]
  inside = lambda u, v, r: not (leaves_allowed(u, v, r + EDGE))

  circles = []   # (u, v, r)
  for (u, v), n in zip(sites, nn):
    x, tt = rng.random(), ts(u, v)
    if tt < 0.2:
      d = 0.9 if x < tt / 0.2 else 0
    else:
      d = 0.9 + (max(0.9, n - GAP) - 0.9) * min(1, (tt - 0.2) / 0.4)
    r = fit(d / 2)
    if r and inside(u, v, r):
      circles.append((u, v, r))

  # small circles into the gaps, brighter half only
  cell = 4.0
  grid = {}
  for c in circles:
    grid.setdefault((int(c[0] // cell), int(c[1] // cell)), []).append(c)
  step = 0.45
  cand = [(i * step + rng.uniform(-0.2, 0.2), j * step + rng.uniform(-0.2, 0.2))
          for i in range(int(W / S / step)) for j in range(int(H / S / step))]
  rng.shuffle(cand)
  for u, v in cand:
    u, v = round(u * S) / S, round(v * S) / S
    tt = ts(u, v)
    if tt < 0.45 or rng.random() > (tt - 0.45) / 0.15:
      continue
    want = 0.45 + 0.2 * min(1, (tt - 0.45) / 0.4)
    room = want
    ci, cj = int(u // cell), int(v // cell)
    for a in (ci - 1, ci, ci + 1):
      for b in (cj - 1, cj, cj + 1):
        for pu, pv, pr in grid.get((a, b), ()):
          room = min(room, math.hypot(pu - u, pv - v) - pr - 0.7)
    room = fit(room)
    if not room or not inside(u, v, room):
      continue
    c = (u, v, room)
    circles.append(c)
    grid.setdefault((ci, cj), []).append(c)

  return circles


def c12_circles():
  """C4's crumpled tissue in light circles on dark ground, no light ground.
  Big circles sit on a Poisson site set 2.4 to 2.7 mm apart and grow with the
  light from 0.9 mm to the room their neighbours leave less 0.65 mm, thinning
  to bare dark in the shadows. Above tone 0.45, circles of 0.94 to 1.3 mm fill
  the gaps wherever a 0.7 mm dark gap to every neighbour remains. Every circle
  is stamped whole, at a radius the print opening leaves untouched."""
  light = np.zeros((H, W), bool)
  for u, v, r in c12_circle_list():
    stamp(light, u, v, r)
  return light


# --- Output -------------------------------------------------------------------

CANDIDATES = [
  ('C0', c0_smoothed, 'TODO', 'TODO'),
  ('C1', c1_coarse, 'TODO', 'TODO'),
  ('C2', c2_grid_folded, 'TODO', 'TODO'),
  ('C3', c3_balled, 'TODO', 'TODO'),
  ('C4', c4_tissue, 'TODO', 'TODO'),
  ('C5', c5_contours,
   'soft tissue crumple drawn as swirling topographic contour bands about 3.2 mm apart',
   'light from the upper right swells each band from 1.0 mm in shadow to solid on the lit crowns'),
  ('C6', c6_swirl_moire,
   'the same tissue swirled into whorls, its contour bands beating in moire against ruled '
   'lines 5 mm apart',
   'light from the upper right on the swirled folds swells the bands from 1.0 mm in shadow to '
   'solid on the lit crowns'),
  ('C7', c7_spiral_drip,
   'the swirled tissue melting, its bands dripping in tongues and winding into spirals that '
   'tighten toward the crowns',
   'light from the upper right, dripped with the bands, swells them from 1.0 mm in shadow to '
   'solid on the lit crowns'),
  ('C8', c8_shards,
   'soft tissue crumple drawn as a mosaic of flat-sided polygon shards about 3.7 mm across',
   'light from the upper right grows light shards out of the dark into tiles parted by 0.76 mm '
   'channels, then turns the ground light under dark chips that shrink away on the crowns'),
  ('C9', c9_shattered,
   'soft tissue crumple shattered into straight-edged splinters, wedges and slivers stretched '
   'along the folds, about 3 by 7 mm in shadow to 6 by 13 mm on the crowns, broken by short '
   'cracks that kink every 2.5 to 4.5 mm, branch and taper to points',
   'light from the upper right grows needles out of the dark into wedges that narrow toward '
   'shadow, parted by torn channels 0.6 to 2 mm wide, then turns the ground light under dark '
   'chips and needles that vanish on the crowns'),
  ('C9a', c9a_shattered,
   'soft tissue crumple shattered into straight-edged splinters, wedges and slivers stretched '
   'along the folds, about 3 by 7 mm in shadow to 6 by 13 mm on the crowns, every edge bent '
   'so no dark run goes straight past about 10 mm, broken by short cracks that kink at every '
   '2 to 4 mm, branch and taper to points',
   'light from the upper right grows needles out of the dark into wedges that narrow toward '
   'shadow, parted by torn channels 0.6 to 2 mm wide, then turns the ground light under dark '
   'chips and needles that vanish on the crowns'),
  ('C10', c10_shattered,
   'tight-balled crumple gone dark: flat-sided folds ending in points, each face lit or dark '
   'whole, broken by kinked cuts into blades and thorns and shattered by five struck-glass '
   'impacts whose broken cracks kink every 1.2 to 3.6 mm and splinter the light outward round '
   'a dark hub',
   'low light from the left lights only the faces turned squarely to it, about 31 % of the '
   'field; faces mostly in the 12 mm cast shadows go dark, and thorn chips and dark slashes '
   'fly outward from each impact'),
  ('C12', c12_circles,
   'C4 tissue crumple drawn in nothing but separate light circles on dark ground, 0.94 to '
   '2.2 mm across, no light ground',
   'light from the upper right grows circles out of bare dark shadow, 0.65 mm apart at the '
   'largest, then packs smaller circles into the gaps on the bright side'),
]


def main(names):
  OUT.mkdir(parents=True, exist_ok=True)
  chosen = [c for c in CANDIDATES if not names or c[0] in names]
  lines = []
  for name, fn, what, how in chosen:
    _, changed = render(name, fn)
    lines.append(f'{name}: {what}. Highlight and shadow: {how}. '
                 f'Printability pass changed {changed * 100:.2f} % of light pixels.')
  print('\n'.join(lines))


if __name__ == '__main__':
  main(sys.argv[1:])
