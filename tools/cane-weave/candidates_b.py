#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family B: organic forms that are not woven.

Each candidate models a height field, lights it from the upper left, and turns
the light into printable two-tone marks. Each candidate function returns the
light-inlay mask on the face grid: 10 px/mm, 1920 x 1520 px, row 0 at the top
edge of the face (y = 3.5 mm in lid face coordinates), column 0 at x = -3.5 mm.

  candidates_b.py           every candidate as <name>.png; prints the report lines
  candidates_b.py B0 B3     only the named candidates
"""

import math
import random
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build' / 'cane' / 'candidates'

# Face geometry in lid face coordinates, mm (the numbers in weave.proof()).

FACE = (-3.5, -148.5, 188.5, 3.5)
FIELD = (-0.5, -145.5, 185.5, 0.5)
HOLES = ([(8.5 + 24 * i, -85.0, 6.25) for i in range(8)]
         + [(8.5 + 24 * i, -108.5, 8.8) for i in range(8)]
         + [(8.5 + 24 * i, -134.5, 8.8) for i in range(8)]
         + [(8.5, -32.0, 6.25), (176.5, -31.5, 8.8), (117.94, -40.75, 7.97)])
OLED = (62.535, -48.75, 97.035, -31.5)      # the window through the lid (owner 2026-11-04)
CLEAR = 1.0
EDGE_CLEAR = 0.2        # mm; a pattern run to the hole edges stays this far off each wall

S = 10                                    # px per mm
W, H = int((FACE[2] - FACE[0]) * S), int((FACE[3] - FACE[1]) * S)
DARK, LIGHT, VOID = (40, 30, 24), (196, 150, 96), (255, 255, 255)

# Narrowest marks drawn, mm: the 0.8 and 0.6 mm print limits plus one pixel.

MIN_LIGHT = 0.9
MIN_DARK = 0.7

# Face grid: u right and v down, mm from the face's top-left corner, at pixel centres.

U, V = np.meshgrid((np.arange(W) + 0.5) / S, (np.arange(H) + 0.5) / S)
COLUMN_U = 8.5 - FACE[0]                 # first hole column; the rest follow every 24 mm


def to_uv(x, y):
  return x - FACE[0], FACE[3] - y


def face_masks(clear=CLEAR):
  """Where inlay may go (the field less a clearance ring of `clear` mm round
  each hole and the window), and the through-holes, drawn white."""
  u0, v0 = to_uv(FIELD[0], FIELD[3])
  u1, v1 = to_uv(FIELD[2], FIELD[1])
  allowed = (U >= u0) & (U <= u1) & (V >= v0) & (V <= v1)
  void = np.zeros(U.shape, bool)
  for x, y, dia in HOLES:
    hu, hv = to_uv(x, y)
    d2 = (U - hu) ** 2 + (V - hv) ** 2
    allowed &= d2 > (dia / 2 + clear) ** 2
    void |= d2 <= (dia / 2) ** 2
  ou0, ov0 = to_uv(OLED[0], OLED[3])
  ou1, ov1 = to_uv(OLED[2], OLED[1])
  allowed &= ~((U >= ou0 - clear) & (U <= ou1 + clear)
               & (V >= ov0 - clear) & (V <= ov1 + clear))
  void |= (U >= ou0) & (U <= ou1) & (V >= ov0) & (V <= ov1)
  return allowed, void


ALLOWED, VOID_MASK = face_masks()
ALLOWED_TO_EDGES, _ = face_masks(EDGE_CLEAR)


# --- Shared tools ---------------------------------------------------------

def wave_noise(u, v, seed, scale, n=7):
  """Smooth noise of unit spread: plane waves with wavelengths near `scale` mm."""
  rng = random.Random(seed)
  out = np.zeros(np.broadcast(u, v).shape)
  for _ in range(n):
    a = rng.uniform(0, 2 * math.pi)
    k = 2 * math.pi / (scale * rng.uniform(0.6, 1.5))
    out += np.sin(k * (u * math.cos(a) + v * math.sin(a)) + rng.uniform(0, 2 * math.pi))
  return out * math.sqrt(2 / n)


def hash01(*keys):
  """A repeatable value in [0, 1) for each tuple of integer keys (arrays welcome)."""
  keys = [np.asarray(k, dtype=np.int64) for k in keys]
  x = np.full(np.broadcast_shapes(*(k.shape for k in keys)), 1469598103934665603, np.uint64)
  with np.errstate(over='ignore'):
    for k in keys:
      x = (x ^ k.astype(np.uint64)) * np.uint64(1099511628211)
    x ^= x >> np.uint64(29)
    x *= np.uint64(0xBF58476D1CE4E5B9)
    x ^= x >> np.uint64(32)
  return (x >> np.uint64(40)).astype(np.float64) / float(1 << 24)


LIGHT_DIR = np.array([-1.0, -1.0, 1.3]) / np.linalg.norm([-1.0, -1.0, 1.3])


def lambert(h):
  """Cosine between the surface normal of height field h (mm) and the light
  from the upper left; a flat face gives 0.68."""
  hv, hu = np.gradient(h, 1 / S)
  lu, lv, lz = LIGHT_DIR
  return np.clip((lz - lu * hu - lv * hv) / np.sqrt(1 + hu * hu + hv * hv), 0, 1)


def cast_shadow(h, slope, reach):
  """Pixels whose ray toward the upper-left light, rising `slope` mm per mm,
  meets the height field within `reach` mm."""
  out = np.zeros(h.shape, bool)
  step = math.sqrt(2) / S
  for k in range(1, int(reach / step) + 1):
    out[k:, k:] |= h[:-k, :-k] > h[k:, k:] + k * step * slope
  return out


def equalize(lit, where=None):
  """Rank of each pixel's light among the pixels in `where`, 0 to 1, so a
  tone threshold picks a fixed share of the area."""
  sample = np.sort((lit if where is None else lit[where]).ravel())
  return np.searchsorted(sample, lit) / sample.size


def blur(a, radius):
  """Three box passes each way, each reaching `radius` mm either side: close
  to a Gaussian, so thresholds drawn from the result have smooth edges."""
  n = max(1, int(radius * S))
  for axis in (0, 1):
    for _ in range(3):
      p = np.pad(a, [(n, n) if ax == axis else (0, 0) for ax in (0, 1)], mode='edge')
      c = np.cumsum(p, axis=axis)
      c = np.insert(c, 0, 0, axis=axis)
      hi = np.take(c, np.arange(2 * n + 1, c.shape[axis]), axis=axis)
      lo = np.take(c, np.arange(0, c.shape[axis] - 2 * n - 1), axis=axis)
      a = (hi - lo) / (2 * n + 1)
  return a


def line_dist(s, period, offset=0.0):
  """Distance, in the units of s, from s to the nearest line at offset + k * period."""
  f = (s - offset) / period
  return np.abs(f - np.round(f)) * period


def line_marks(dist, period, t, lo=0.12, hi=0.9):
  """Light where `dist` (mm from the nearest line centre) is under half the
  line width. The width follows tone t but takes only printable values: none,
  MIN_LIGHT up to period - MIN_DARK, or solid (hi=None never goes solid)."""
  k = np.clip((t - lo) / ((hi or 1.0) - lo), 0, 1)
  w = MIN_LIGHT + k * (period - MIN_DARK - MIN_LIGHT)
  w = np.where(t <= lo, 0.0, w)
  if hi is not None:
    w = np.where(t >= hi, 4 * period, w)
  return dist < w / 2


# --- Printability pass ----------------------------------------------------

def disc(d):
  """Pixel offsets of a disc d px across, centred between pixels."""
  r = d / 2
  return [(dy, dx) for dy in range(-(d // 2), d // 2) for dx in range(-(d // 2), d // 2)
          if (dy + 0.5) ** 2 + (dx + 0.5) ** 2 <= r * r]


def erode(m, offs):
  r = max(max(abs(a), abs(b)) for a, b in offs)
  p = np.pad(m, r, mode='edge')
  out = np.ones_like(m)
  for dy, dx in offs:
    out &= p[r + dy:r + dy + m.shape[0], r + dx:r + dx + m.shape[1]]
  return out


def dilate(m, offs):
  r = max(max(abs(a), abs(b)) for a, b in offs)
  p = np.pad(m, r)
  out = np.zeros_like(m)
  for dy, dx in offs:
    out |= p[r - dy:r - dy + m.shape[0], r - dx:r - dx + m.shape[1]]
  return out


def opening(m, offs):
  return dilate(erode(m, offs), offs)


def closing(m, offs):
  return ~opening(~m, offs)


OPEN_DISC = disc(8)     # light features narrower than 0.8 mm vanish
CLOSE_DISC = disc(6)    # dark gaps narrower than 0.6 mm fill


def printable(light, region=ALLOWED):
  """Opening by an 8 px disc, then closing by a 6 px disc, repeated until the
  mask stops changing. Returns the mask and the fraction of the original light
  pixels that changed (removed or added)."""
  start = light & region
  m = start
  for _ in range(6):
    nxt = closing(opening(m, OPEN_DISC), CLOSE_DISC) & region
    if np.array_equal(nxt, m):
      break
    m = nxt

  # A neck too thin to print can leave, once removed, a gap too thin to
  # print, which closing then refills. Such conflicts resolve to dark: the
  # gap is widened instead. Light only shrinks here, so the loop ends.

  while True:
    m = opening(m, OPEN_DISC)
    thin = closing(m, CLOSE_DISC) & ~m
    if not thin.any():
      break
    m &= ~dilate(thin, CLOSE_DISC)
  assert np.array_equal(opening(m, OPEN_DISC), m)
  assert np.array_equal(closing(m, CLOSE_DISC) & region, m)
  return m, np.count_nonzero(m != start) / np.count_nonzero(start)


# --- B0: overlapping scales -------------------------------------------------

def b0_scales():
  """Fish scales, free edge down, 12 mm wide in 5.6 mm rows, repeating every
  24 mm across; the hole columns fall on scale centres. Each scale is a dome
  whose crown sits toward its free edge, and the edge of the scale above casts
  a crescent of shadow on it. Arcs parallel to the free edge carry the shading."""
  pitch, row, rad, v0 = 12.0, 5.6, 6.6, 1.5
  r_base = np.round((V - v0) / row).astype(np.int64)

  def candidates():
    for dr in range(-2, 3):
      r = r_base + dr
      shift = 6.0 * np.mod(r, 2)
      c_base = np.round((U - COLUMN_U - shift) / pitch).astype(np.int64)
      for dc in (-1, 0, 1):
        c = c_base + dc
        odd = np.mod(c, 2)
        cu = COLUMN_U + shift + c * pitch + 0.8 * (hash01(r, odd, 1) - 0.5)
        cv = v0 + r * row + 1.0 * (hash01(r, odd, 2) - 0.5)
        rr = rad + 0.6 * (hash01(r, odd, 3) - 0.5)

        # Upper rows lie on top; within a row the right scale overlaps the left.

        yield r - 0.001 * c, cu, cv, rr

  top = np.full(U.shape, np.inf)
  cu_w, cv_w, r_w = np.zeros(U.shape), np.zeros(U.shape), np.ones(U.shape)
  for order, cu, cv, rr in candidates():
    win = (np.hypot(U - cu, V - cv) < rr) & (order < top)
    top = np.where(win, order, top)
    cu_w, cv_w, r_w = np.where(win, cu, cu_w), np.where(win, cv, cv_w), np.where(win, rr, r_w)

  # Distance below the free edge of the overlying scales, mm, and how
  # squarely that edge faces away from the light (1 where it faces lower right).

  under = np.full(U.shape, np.inf)
  away = np.zeros(U.shape)
  for order, cu, cv, rr in candidates():
    d = np.hypot(U - cu, V - cv)
    nearer = (order < top) & (d - rr < under)
    under = np.where(nearer, d - rr, under)
    away = np.where(nearer, ((U - cu) + (V - cv)) / (math.sqrt(2) * d), away)

  a, b = (U - cu_w) / r_w, (V - cv_w) / r_w
  nd = np.hypot(a, b)
  q = 1 - a * a - ((b - 0.35) / 0.75) ** 2
  h = 2.2 * np.sqrt(np.clip(q, 0, None)) - 0.3 * np.clip((nd - 0.85) / 0.15, 0, 1) ** 2
  t = blur(equalize(lambert(h)), 0.3)

  # The overlying edge casts its shadow down and to the right: 0.6 mm deep
  # where the edge faces the light, 2 mm where it faces away.

  shadow = under < 0.6 + 1.4 * np.clip(away, 0, 1)

  # The first arc rides the free edge and never fades, so every scale keeps
  # its outline even where its body turns into shadow.

  inward = r_w * (1 - nd)
  t = np.where(inward < 1.2, np.maximum(t, 0.2), t)
  light = line_marks(line_dist(inward, 2.4, 0.45), 2.4, t, lo=0.15, hi=0.55)
  return light & (inward > 0) & ~shadow & np.isfinite(top)


# --- B1: river pebbles ------------------------------------------------------

def b1_pebbles():
  """River pebbles about 8 to 14 mm across, packed at random with gaps of
  0.9 to 1.6 mm. Each pebble is a dome; its lower-right side and the gap
  beyond it fall into shadow. Diagonal hatching that bows over each dome
  carries the shading."""
  rng = random.Random(7)

  def radius(u, v):
    return float(np.clip(5.6 + 1.6 * wave_noise(u, v, 12, 80), 4.4, 7.8))

  sites, buckets, cell = [], {}, 16.0
  for _ in range(60000):
    u, v = rng.uniform(-8, W / S + 8), rng.uniform(-8, H / S + 8)
    r = radius(u, v)
    bi, bj = int(u // cell), int(v // cell)
    if all((pu - u) ** 2 + (pv - v) ** 2 >= (0.92 * (pr + r)) ** 2
           for i in (bi - 1, bi, bi + 1) for j in (bj - 1, bj, bj + 1)
           for pu, pv, pr in buckets.get((i, j), ())):
      sites.append((u, v, r))
      buckets.setdefault((bi, bj), []).append((u, v, r))
  pts = np.array([(u, v) for u, v, _ in sites])

  # Each pebble is its Voronoi cell, inset and rounded by a soft minimum of
  # the distances to the cell's edges.

  hgt = np.zeros(U.shape)
  body = np.zeros(U.shape, bool)
  half, soft_k = 18.0, 1.4
  for n, (pu, pv, _) in enumerate(sites):
    i0, i1 = max(0, int((pu - half) * S)), min(W, int((pu + half) * S))
    j0, j1 = max(0, int((pv - half) * S)), min(H, int((pv + half) * S))
    if i0 >= i1 or j0 >= j1:
      continue
    uu, vv = U[j0:j1, i0:i1], V[j0:j1, i0:i1]
    d = np.hypot(pts[:, 0] - pu, pts[:, 1] - pv)
    near = np.nonzero((d > 0) & (d < 2 * half))[0]
    nu, nv = (pu - pts[near, 0]) / d[near], (pv - pts[near, 1]) / d[near]
    mu, mv = (pu + pts[near, 0]) / 2, (pv + pts[near, 1]) / 2
    edge = nu[:, None, None] * (uu - mu[:, None, None]) + nv[:, None, None] * (vv - mv[:, None, None])
    emin = edge.min(0)
    soft = emin - soft_k * np.log(np.exp(-(edge - emin) / soft_k).sum(0))
    gap = 0.45 + 0.35 * rng.random()
    inside = (emin > 0) & (soft > gap)
    if not inside.any():
      continue
    q = np.clip((soft - gap) / (soft[inside].max() - gap), 0, 1)
    peak = (0.42 + 0.2 * rng.random()) * (soft[inside].max() - gap)
    dome = peak * (1 - (1 - q) ** 2)
    hgt[j0:j1, i0:i1] = np.where(inside, dome, hgt[j0:j1, i0:i1])
    body[j0:j1, i0:i1] |= inside

  t = equalize(lambert(hgt), body)
  t[cast_shadow(hgt, 1.0, 3.0)] = 0
  t = blur(t, 0.5)
  t[~body] = 0

  # Diagonal hatching, as if seen a little from the lower right: each line
  # rides up over a pebble by 0.45 of its height, so the lines bow over the
  # domes. True line spacing comes from the gradient of the warped coordinate.

  across = (U + V) / math.sqrt(2) - 0.45 * hgt
  gv, gu = np.gradient(across, 1 / S)
  stretch = np.hypot(gu, gv)
  period = 2.3 / stretch
  dist = line_dist(across, 2.3) / stretch
  return line_marks(dist, period, t, lo=0.1, hi=0.85) & body


# --- B2: weathered wood grain -----------------------------------------------

B2_KNOTS = [(38.0, 62.0, 3.6, 2.3), (150.0, 20.0, 4.4, 2.6), (72.0, 100.0, 2.6, 2.0)]


def b2_grain():
  """Weathered wood: hard growth rings stand as ridges about 2 to 6.5 mm apart,
  flowing around three raised knots. One light line rides each ridge; its
  width follows the light, so it swells on the lit flank and thins where the
  ridge turns away. The knots carry rings of their own."""
  tilt = math.radians(-6)
  y = V * math.cos(tilt) - U * math.sin(tilt)
  y = (y + 4.0 * wave_noise(U, V, 21, 120) + 2.0 * wave_noise(U, V, 22, 45)
       + 0.6 * wave_noise(U, V, 23, 18))
  knot_r = np.full(U.shape, np.inf)
  knot_rho = np.ones(U.shape)
  for ku, kv, rho, ex in B2_KNOTS:
    dx, dy = (U - ku) / ex, V - kv
    r2 = dx * dx + dy * dy

    # Rings part around the knot like a stream around a stone.

    y = y - 0.7 * dy * rho * rho / np.maximum(r2, rho * rho)
    kr = np.sqrt(r2) / rho
    knot_rho = np.where(kr < knot_r, rho, knot_rho)
    knot_r = np.minimum(knot_r, kr)

  ring = y / 3.4 + 0.3 * np.sin(y / 9.0) + 0.15 * np.sin(y / 4.3 + 1.0)
  gv, gu = np.gradient(ring, 1 / S)
  period = 1 / np.hypot(gu, gv)
  frac = ring - np.round(ring)
  crest = np.abs(frac) * period

  # Each growth ring stands to its own height, in proportion to its spacing
  # so close rings do not bury each other in shadow.

  rise = 0.3 * period * (0.5 + hash01(np.round(ring).astype(np.int64), 5))
  h = rise * np.cos(np.pi * frac) ** 4

  knot = knot_r < 1
  h = np.where(knot, 0.5 + 0.9 * np.sqrt(np.clip(1 - knot_r ** 2, 0, 1)), h)

  # Light is judged against its 4 mm neighbourhood, so a stretch of grain
  # turned away from the light still shows a lit and a shaded flank on every ridge.

  lit = lambert(h)
  t = blur(equalize(lit - blur(lit, 4.0)), 0.2)
  t[cast_shadow(h, 0.5, 2.0)] = 0

  grain = line_marks(crest, period, t, lo=0.05, hi=None)
  rings = line_marks(line_dist((1 - knot_r) * knot_rho, 2.0, 1.1), 2.0, t, hi=0.97)
  light = np.where(knot, rings, grain)
  return light & ~((knot_r >= 1) & (knot_r < 1 + 0.8 / knot_rho)) & (knot_r > 0.3)


# --- B3: wind ripples in sand -------------------------------------------------

B3_DEFECTS = [(28, 112, 1), (78, 34, -1), (122, 124, 1), (163, 72, -1),
              (100, 12, 1), (52, 142, -1), (140, 40, 1)]


def b3_ripples():
  """Wind ripples in sand, crests about 11 mm apart, running from upper right
  to lower left, meandering and forking. Each ripple has a longer slope facing
  the light and a steeper lee in shadow. Horizontal hatching carries the
  shading: its lines swell on the lit slopes and taper away into the lee."""
  th = math.radians(30)
  psi = (U * math.cos(th) + V * math.sin(th)
         + 4.5 * wave_noise(U, V, 31, 80) + 1.2 * wave_noise(U, V, 32, 26))
  phase = psi / 11.0
  for du, dv, s in B3_DEFECTS:
    phase = phase + s * np.arctan2(V - dv, U - du) / (2 * math.pi)

  # Skewing the phase squeezes the falling half of each wave: the slope
  # facing the upper-left light is long, the lee facing lower right is steep.

  f = np.mod(phase, 1.0)
  h = -1.3 * np.cos(2 * math.pi * (f + 0.07 * np.cos(2 * math.pi * f)))
  t = blur(equalize(lambert(h)), 0.2)
  t[cast_shadow(h, 0.6, 2.5)] = 0
  s = V + 0.6 * wave_noise(U, V, 33, 45)
  return line_marks(line_dist(s, 2.2), 2.2, t, lo=0.1, hi=None)


# --- B4: quilted diamonds -----------------------------------------------------

def b4_quilt():
  """Tufted diamond pillows 24 mm wide and about 25 mm tall, repeating every
  24 mm across. The tufts fall on the knob holes, so each knob sits where
  four pillows pinch in, like a button; tufts with no knob get a small
  button. Each pillow bulges, lit from the upper left, with a shadow into
  the seam below it. Echo-quilting rings carry the shading."""

  # Pillow rows (diamond heights) through v: the hole rows land on tuft rows.

  rows_v = [-60.0, 35.25, 88.5, 112.0, 138.0, 260.0]
  rows_t = [1 - 95.25 / 25, 1.0, 3.0, 4.0, 5.0, 5 + 122 / 25]
  rows = np.interp(V, rows_v, rows_t)
  du = U - COLUMN_U

  # Wobble that vanishes on the hole columns keeps the tufts on the knobs.

  wob = np.sin(2 * math.pi * du / 24)
  across = du / 24 + 0.5 + 0.04 * wob * wave_noise(U, V, 41, 60)
  rows = rows + 0.03 * wob * wave_noise(U, V, 42, 70)
  a, b = across + rows, across - rows
  ia, ib = np.round(a), np.round(b)
  fa, fb = a - ia, b - ib
  puff = 3.2 * (0.85 + 0.3 * hash01((ia - ib).astype(np.int64), np.mod(ia + ib, 2).astype(np.int64)))
  m = (np.abs(2 * fa) ** 3.5 + np.abs(2 * fb) ** 3.5) ** (1 / 3.5)
  h = puff * np.clip(1 - m * m, 0, 1) ** 0.6
  t = blur(equalize(lambert(h)), 0.3)
  t[cast_shadow(h, 0.7, 3.0)] = 0

  # Across a pillow the facing edges are 17.7 mm apart, so near an edge
  # midpoint m rises 2 / 17.7 per mm and (1 - m) * 8.85 is mm from the seam.

  seam = (1 - m) * 8.85

  # The crown of each pillow catches the light whole, so no ring closes on itself there.

  t = np.where(seam > 6.4, np.maximum(t, 0.7), t)
  light = line_marks(line_dist(seam, 2.2, 0.85), 2.2, t, lo=0.15, hi=0.65) & (seam > 0.4)

  # A button at each tuft, where four pillows meet.

  corner = np.hypot((0.5 - np.abs(fa)) * 17.7, (0.5 - np.abs(fb)) * 17.7)
  return light | (corner < 1.1)


# --- Output -------------------------------------------------------------------

CANDIDATES = [
  ('B0', b0_scales,
   'overlapping fish scales, 12 mm wide in 5.6 mm rows, repeat 24 mm across, hole columns '
   'on scale centres',
   'each scale a dome; arcs 2.4 mm apart run parallel to its free edge, 0.9 to 1.7 mm wide, '
   'merging solid on the lit upper left and thinning to the lower right; the arc on the free '
   'edge never fades, so every outline holds; the edge above casts a dark crescent 0.6 mm '
   'deep on the lit side, 2.0 mm at the lower right'),
  ('B1', b1_pebbles,
   'river pebbles about 8 to 14 mm across, packed at random, no repeat',
   'each pebble a dome; diagonal hatching 2.3 mm apart (lower left to upper right) rides up '
   'over each dome by 0.45 of its height, so the lines bow with the form; lines run 0.9 mm '
   'up to the spacing less 0.7 mm, solid in the brightest 15 % of the pebble area; the '
   'lower-right side and the cast shadow (light at 45 degrees) are dark and merge with the '
   'gap, which is 0.9 to 1.6 mm wide'),
  ('B2', b2_grain,
   'weathered wood grain, ridges about 2 to 6.5 mm apart flowing round three raised knots, '
   'no repeat',
   'one light line rides each ridge, 0.9 mm on the shaded flank up to the ridge spacing less '
   '0.7 mm on the lit flank, so the lines swell and thin as the grain turns; ridge heights '
   'vary ring to ring; knots are domes with their own rings inside a 0.8 mm dark moat'),
  ('B3', b3_ripples,
   'wind ripples in sand, crests about 11 mm apart running upper right to lower left, '
   'meandering and forking, no repeat',
   'horizontal hatching 2.2 mm apart, 0.9 to 1.5 mm wide on the slopes facing the light, '
   'tapering away on the turn; the steeper lee and its cast shadow make dark bands'),
  ('B4', b4_quilt,
   'tufted diamond pillows 24 mm wide by 23.5 to 26.6 mm tall, repeat 24 mm across; the '
   'knob holes sit on the tufts like buttons, and tufts with no knob get a 2.2 mm button',
   'each pillow a cushion with a solid crown; echo-quilting rings 2.2 mm apart, 0.9 to 1.5 mm '
   'wide, go solid on the lit upper left and thin to the lower right; the seam is 0.8 mm '
   'and widens into the cast shadow on the lower-right side of each pillow'),
]


def face_image(light):
  img = np.empty((H, W, 3), np.uint8)
  img[:] = DARK
  img[light] = LIGHT
  img[VOID_MASK] = VOID
  return Image.fromarray(img)


def zoom(img):
  """60 x 60 mm from the centre of the field, scaled 2x."""
  cu, cv = to_uv((FIELD[0] + FIELD[2]) / 2, (FIELD[1] + FIELD[3]) / 2)
  ci, cj, half = round(cu * S), round(cv * S), 30 * S
  return img.crop((ci - half, cj - half, ci + half, cj + half)).resize((1200, 1200), Image.NEAREST)


def render(name, fn):
  t0 = time.time()
  light, changed = printable(fn())
  img = face_image(light)
  img.save(OUT / f'{name}.png')
  print(f'{name}: {changed * 100:.2f} % of light pixels changed, {time.time() - t0:.1f} s')
  return img, changed


def main(names):
  OUT.mkdir(parents=True, exist_ok=True)
  chosen = [c for c in CANDIDATES if not names or c[0] in names]
  results, lines = [], []
  for name, fn, what, how in chosen:
    img, changed = render(name, fn)
    results.append((name, img))
    lines.append(f'{name}: {what}. Highlight and shadow: {how}. '
                 f'Printability pass changed {changed * 100:.2f} % of light pixels.')
  print('\n'.join(lines))


if __name__ == '__main__':
  main(sys.argv[1:])
