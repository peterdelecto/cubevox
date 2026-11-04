#!/usr/bin/env python3
"""Candidate lid-face inlay textures, round 2, letter F: five readings of asphalt.

Each candidate models a road surface as a height field (crushed stones, pits,
crack valleys, a sealant ribbon, a worn wheel path), lights it from the upper
left, and draws the light with printable two-tone marks. Each candidate
function returns the light-inlay mask on the face grid of candidates_b.py:
10 px/mm, 1920 x 1520 px. The face grid, the printability pass and the line
tools come from candidates_b.py.

  candidates_f.py           every candidate as <name>.png; prints the report lines
  candidates_f.py F0 F3     only the named candidates
"""

import math
import random
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))

from candidates_b import (  # noqa: E402
  H, OUT, S, U, V, W, blur, equalize, face_image, hash01, line_dist, line_marks,
  printable, wave_noise)


# --- Light ----------------------------------------------------------------

def light_dir(azimuth, elevation):
  """Unit vector toward the light in (u right, v down, z up). Azimuth is in
  degrees from the left edge toward the top edge, so 45 is the upper left;
  elevation is in degrees above the face."""
  a, e = math.radians(azimuth), math.radians(elevation)
  return np.array([-math.cos(e) * math.cos(a), -math.cos(e) * math.sin(a), math.sin(e)])


def lambert(h, light):
  """Cosine between the surface normal of height field h (mm) and the light."""
  hv, hu = np.gradient(h, 1 / S)
  lu, lv, lz = light
  return np.clip((lz - lu * hu - lv * hv) / np.sqrt(1 + hu * hu + hv * hv), 0, 1)


def cast_shadow(h, light, reach):
  """Pixels whose ray toward the light meets the height field within `reach` mm."""
  flat = math.hypot(light[0], light[1])
  du, dv, slope = light[0] / flat, light[1] / flat, light[2] / flat
  lead = max(abs(du), abs(dv))
  pad = int(reach * S) + 2
  p = np.pad(h, pad, constant_values=-np.inf)
  out = np.zeros(h.shape, bool)
  for k in range(1, int(reach * S * lead) + 1):
    t = k / lead
    oi, oj = round(t * du), round(t * dv)
    out |= p[pad + oj:pad + oj + H, pad + oi:pad + oi + W] > h + t / S * slope
  return out


# --- Aggregate ------------------------------------------------------------

def place_stones(seed, grades, gap):
  """Graded aggregate by dart throwing. Each grade is (r_lo, r_hi, tries, sign),
  largest first; a stone keeps `gap` mm clear of every stone placed before it.
  Sign -1 marks a pit. Returns (u, v, r, sign) per stone, mm."""
  rng = random.Random(seed)
  cell = 2 * max(g[1] for g in grades) + gap
  buckets, out = {}, []
  for r_lo, r_hi, tries, sign in grades:
    for _ in range(tries):
      u, v = rng.uniform(-r_hi, W / S + r_hi), rng.uniform(-r_hi, H / S + r_hi)
      r = rng.uniform(r_lo, r_hi)
      bi, bj = int(u // cell), int(v // cell)
      if all((pu - u) ** 2 + (pv - v) ** 2 >= (pr + r + gap) ** 2
             for i in (bi - 1, bi, bi + 1) for j in (bj - 1, bj, bj + 1)
             for pu, pv, pr in buckets.get((i, j), ())):
        out.append((u, v, r, sign))
        buckets.setdefault((bi, bj), []).append((u, v, r))
  return out


def stone_cells(stones, seed, gap, peak, gamma, soft=0.0, bulge=1.3, sides=(5, 8), squash=1.0):
  """Height field of crushed stones, mm. Each stone fills its cell of the
  power diagram of the placed circles, inset gap/2 from every cell edge and
  kept inside a random polygon with `sides` sides about `bulge` times its
  radius, squashed along a random axis to between `squash` and 1 of its
  length, so the binder shows as seams where stones crowd and as angular
  voids where they leave room. A stone rises to a faceted crown `peak` times
  its radius high (gamma 1 gives flat facets, lower rounds them); `soft` (mm)
  rounds its corners. A pit (sign -1) is the same shape sunk into the binder.
  Returns the height, the index of the stone at each pixel (-1 for binder),
  the depth inside that stone's outline (mm) and its radius (mm)."""
  pts = np.array([s[:3] for s in stones])
  rng = np.random.default_rng(seed)
  hgt = np.zeros((H, W))
  top = np.full((H, W), -1, np.int32)
  depth, rad = np.zeros((H, W)), np.zeros((H, W))
  for n, (su, sv, r, sign) in enumerate(stones):
    rise = sign * rng.uniform(*peak) * r
    reach = bulge * r
    k = int(rng.integers(sides[0], sides[1] + 1))
    th = (rng.uniform(0, 2 * math.pi) + 2 * math.pi * np.arange(k) / k
          + rng.uniform(-0.4, 0.4, k) * 2 * math.pi / k)
    rk = reach * rng.uniform(0.72, 1.0, k)
    turn, sq = rng.uniform(0, math.pi), rng.uniform(squash, 1.0)
    i0, i1 = max(0, int((su - reach) * S)), min(W, int((su + reach) * S) + 2)
    j0, j1 = max(0, int((sv - reach) * S)), min(H, int((sv + reach) * S) + 2)
    if i0 >= i1 or j0 >= j1:
      continue
    x, y = U[j0:j1, i0:i1] - su, V[j0:j1, i0:i1] - sv
    a = x * math.cos(turn) + y * math.sin(turn)
    b = (y * math.cos(turn) - x * math.sin(turn)) / sq
    d = np.hypot(pts[:, 0] - su, pts[:, 1] - sv)
    near = np.nonzero((d > 0) & (d < bulge * (r + pts[:, 2])))[0]
    dn = d[near]
    nu, nv = (pts[near, 0] - su) / dn, (pts[near, 1] - sv) / dn
    off = (dn * dn + r * r - pts[near, 2] ** 2) / (2 * dn) - gap / 2
    edges = np.concatenate([
      sq * (rk[:, None, None] - (a * np.cos(th)[:, None, None] + b * np.sin(th)[:, None, None])),
      off[:, None, None] - (x * nu[:, None, None] + y * nv[:, None, None])])
    e = edges.min(0)
    if soft > 0:
      e = e - soft * np.log(np.exp(-(edges - e) / soft).sum(0))
    emax = e.max()
    if emax <= 0:
      continue
    win = e > 0
    hgt[j0:j1, i0:i1][win] = (rise * np.clip(e / emax, 0, 1) ** gamma)[win]
    top[j0:j1, i0:i1][win] = n
    depth[j0:j1, i0:i1][win] = e[win]
    rad[j0:j1, i0:i1][win] = r
  return hgt, top, depth, rad


def warped_lines(across, period, h, lift):
  """Distance (mm) to the nearest line of `across` = k * period after each line
  rides up over the height field by `lift` of its height, and the true line
  spacing there (mm)."""
  s = across - lift * h
  gv, gu = np.gradient(s, 1 / S)
  stretch = np.maximum(np.hypot(gu, gv), 0.2)
  return line_dist(s, period) / stretch, period / stretch


def poisson_points(seed, spacing):
  """Blue-noise points over the face at least `spacing` mm apart, mm."""
  rng = random.Random(seed)
  cell = spacing
  buckets, out = {}, []
  for _ in range(int(12 * (W / S) * (H / S) / spacing ** 2)):
    u, v = rng.uniform(0, W / S), rng.uniform(0, H / S)
    bi, bj = int(u // cell), int(v // cell)
    if all((pu - u) ** 2 + (pv - v) ** 2 >= spacing ** 2
           for i in (bi - 1, bi, bi + 1) for j in (bj - 1, bj, bj + 1)
           for pu, pv in buckets.get((i, j), ())):
      out.append((u, v))
      buckets.setdefault((bi, bj), []).append((u, v))
  return out


def stipple(t, seed, spacing, dot, hole, split, solid):
  """Stipple on blue-noise points `spacing` mm apart, each with a random rank.
  Below tone `split` the ground is dark and a point carries a light dot `dot`
  mm across while its rank is under tone / split, so dots thicken with the
  light. From `split` to `solid` the ground is light and a point carries a
  dark hole `hole` mm across while its rank is over the share of the way to
  `solid`, so holes thin out toward it. Above `solid` the light is unbroken."""
  rng = random.Random(seed + 1)
  dots = np.zeros(t.shape, bool)
  holes = np.zeros(t.shape, bool)
  for u, v in poisson_points(seed, spacing):
    i, j = int(u * S), int(v * S)
    tone, rank = t[j, i], rng.random()
    if tone < split:
      if rank >= tone / split:
        continue
      target, r = dots, dot * S / 2
    else:
      if rank < (tone - split) / (solid - split):
        continue
      target, r = holes, hole * S / 2
    n = int(r) + 1
    i0, i1, j0, j1 = max(0, i - n), min(W, i + n + 1), max(0, j - n), min(H, j + n + 1)
    du = (np.arange(i0, i1) + 0.5) / S - u
    dv = (np.arange(j0, j1) + 0.5) / S - v
    target[j0:j1, i0:i1] |= (du[None, :] ** 2 + dv[:, None] ** 2) * S * S <= r * r
  return ((t >= split) & ~holes) | dots


def voronoi(seed, cell, warp, warp_scale):
  """Cells of a jittered grid of seeds `cell` mm apart on a noise-warped face.
  Returns each pixel's cell id, its seed position (mm) and its distance to the
  nearest cell edge (mm)."""
  wu = U + warp * wave_noise(U, V, seed, warp_scale)
  wv = V + warp * wave_noise(U, V, seed + 1, warp_scale)
  gi, gj = np.floor(wu / cell).astype(np.int64), np.floor(wv / cell).astype(np.int64)

  def site(ci, cj):
    return ((ci + 0.1 + 0.8 * hash01(ci, cj, seed, 1)) * cell,
            (cj + 0.1 + 0.8 * hash01(ci, cj, seed, 2)) * cell)

  best = np.full(U.shape, np.inf)
  bi, bj = gi.copy(), gj.copy()
  for di in (-1, 0, 1):
    for dj in (-1, 0, 1):
      su, sv = site(gi + di, gj + dj)
      d = np.hypot(wu - su, wv - sv)
      near = d < best
      best, bi, bj = np.where(near, d, best), np.where(near, gi + di, bi), np.where(near, gj + dj, bj)
  au, av = site(bi, bj)
  edge = np.full(U.shape, np.inf)
  for di in range(-2, 3):
    for dj in range(-2, 3):
      if di == 0 and dj == 0:
        continue
      su, sv = site(bi + di, bj + dj)
      nu, nv = su - au, sv - av
      norm = np.hypot(nu, nv)
      edge = np.minimum(edge, ((su + au) / 2 - wu) * nu / norm + ((sv + av) / 2 - wv) * nv / norm)
  return bi * 100003 + bj, au, av, edge


# --- F0: fresh dense asphalt ------------------------------------------------

def f0_fresh():
  """Fresh dense asphalt: crushed chips 4 to 9 mm across, packed tight with
  0.8 mm binder seams, and a scatter of pits 4 to 5 mm across. Light from
  the upper left, 50 degrees up. Each chip is a faceted pyramid cut in three
  tones like a gem: facets facing the light solid, facets turned side-on
  hatched with 0.9 mm lines 1.6 mm apart across the light, facets turned
  away dark. Each pit shows a light crescent on its lower-right wall, the
  wall that faces the light."""
  light = light_dir(45, 50)
  stones = place_stones(11, [(2.0, 2.5, 500, -1), (3.5, 4.5, 3000, 1),
                             (2.6, 3.5, 6000, 1), (2.0, 2.6, 12000, 1)], gap=0.0)
  h, top, depth, rad = stone_cells(stones, 12, gap=0.8, peak=(0.4, 0.6), gamma=1.0,
                                   sides=(4, 7), squash=0.7)
  lit = lambert(h, light)
  shade = cast_shadow(h, light, 2.5)
  chip = h > 0
  t = equalize(lit, chip)
  hatch = line_dist((U + V) / math.sqrt(2), 1.6) < 0.45
  chips = chip & ((t > 0.66) | ((t > 0.3) & hatch))
  pits = (h < 0) & (lit > 0.9)
  return (chips | pits) & ~shade


# --- F1: coarse open-graded asphalt -----------------------------------------

def f1_open():
  """Open-graded asphalt: big angular stones 6 to 14 mm across with small
  chips between, standing over deep connected voids. Light from the upper
  left, 40 degrees up. Contour rings 1.9 mm apart follow each big stone's
  outline inward; they swell to solid on the facets facing the light and thin
  out on the far side. The voids and the cast shadows are solid dark."""
  light = light_dir(45, 40)
  stones = place_stones(21, [(5.5, 8.0, 3000, 1), (3.5, 5.5, 5000, 1),
                             (1.6, 2.4, 8000, 1)], gap=0.8)
  h, top, depth, rad = stone_cells(stones, 22, gap=1.4, peak=(0.3, 0.42), gamma=1.0, bulge=1.25,
                                   sides=(4, 7), squash=0.6)
  body = top >= 0
  t = equalize(lambert(h, light), body)
  shade = cast_shadow(h, light, 3.0)
  big = rad >= 3.0

  # The outline ring holds on every big stone outside the cast shadow, so
  # each stone keeps its edge where its body turns from the light.

  t = np.where(big & (depth < 1.1), np.maximum(t, 0.3), t)
  rings = line_marks(line_dist(depth, 2.6, 0.55), 2.6, t, lo=0.28, hi=None)
  chips = t > 0.5
  return np.where(big, rings, chips) & body & ~shade


# --- F2: alligator cracking -------------------------------------------------

def f2_alligator():
  """Alligator-cracked asphalt: slabs 6 to 18 mm across, each heaved to its
  own tilt, split by crack valleys 0.8 to 2 mm wide. Low raking light from
  the left, 30 degrees up. Each slab is hatched along its own contour lines,
  1.9 mm apart; the hatch swells to solid on the lips facing the light and
  thins to nothing where a slab rolls down into the shadowed side of a crack."""
  light = light_dir(20, 30)
  id1, su1, sv1, e1 = voronoi(31, 15.0, 0.7, 14.0)
  id2, su2, sv2, e2 = voronoi(32, 7.5, 0.35, 6.0)

  # The slab has broken further in patches: there the finer cracks show too.

  fine = wave_noise(U, V, 33, 70) > -0.3
  w1 = 2.0 + 0.4 * wave_noise(U, V, 34, 25)
  w2 = 1.2 + 0.1 * wave_noise(U, V, 35, 12)
  gap1 = e1 - w1 / 2
  gap2 = np.where(fine, e2 - w2 / 2, np.inf)
  gap = np.minimum(gap1, gap2)
  bid = np.where(fine, id1 * 7919 + id2, id1)
  ref_u, ref_v = np.where(fine, su2, su1), np.where(fine, sv2, sv1)

  phi = 2 * math.pi * hash01(bid, 1)
  tilt = 0.03 + 0.07 * hash01(bid, 2)
  slab = 0.15 * hash01(bid, 3) + tilt * ((U - ref_u) * np.cos(phi) + (V - ref_v) * np.sin(phi))

  # Each slab rolls down into the crack over 1.5 mm; the crack floor is 0.9 mm down.

  roll = np.clip(1 - gap / 1.5, 0, None)
  h = np.where(gap > 0, slab - 0.5 * roll ** 2, slab - 0.5 - 0.4 * np.clip(-gap / 0.4, 0, 1))
  # The slab faces sit in the upper two thirds of the tone range, so they
  # read light against the dark cracks.

  t = 0.3 + 0.7 * blur(equalize(lambert(h, light), gap > 0), 0.15)
  t[cast_shadow(h, light, 2.5)] = 0

  # Each slab is hatched at its own angle, 40 to 80 degrees from the
  # horizontal; the lines bow down where the slab rolls into a crack.

  period = 2.2
  beta = math.radians(40) + math.radians(40) * hash01(bid, 4)
  across = U * np.sin(beta) - V * np.cos(beta) + period * hash01(bid, 5)
  dist, spacing = warped_lines(across, period, h, 0.8)
  marks = line_marks(dist, spacing, t, lo=0.06, hi=0.9)
  return marks & (gap > 0)


# --- F3: patched asphalt with a sealed crack --------------------------------

def crack_path(u):
  """Centre of the sealed crack: v (mm) as a function of u (mm), snaking
  across the face from the left edge to the right between the screen and
  the first row of knobs."""
  return (72 + 10 * np.sin(2 * math.pi * u / 120 + 2.0) + 4 * np.sin(2 * math.pi * u / 37 + 0.5)
          + 0.8 * np.sin(2 * math.pi * u / 10 + 0.3))


def branch_path(u):
  """A branch that leaves the main crack at u = 104 mm, climbs to the upper
  right and dies out by u = 168 mm."""
  return crack_path(104.0) - 0.42 * (u - 104) + 2.5 * np.sin(2 * math.pi * (u - 104) / 23)


def f3_sealed():
  """Weathered asphalt, rounded stones 5 to 9 mm across in open binder, with a
  crack sealed by a ribbon of glossy tar snaking across the face and one
  branch dying out. Light from the top left, high, 55 degrees up. The
  aggregate is stippled on blue-noise points 1.75 mm apart: 1.0 mm light
  dots thicken with the light, then the ground turns light with 0.8 mm dark
  holes that thin out toward each stone's solid crown; none in the cast
  shadow. The tar ribbon is dark with a specular highlight along its lit
  shoulder, split where the tar has sunk into the crack beneath."""
  light = light_dir(65, 55)
  stones = place_stones(41, [(4.0, 5.5, 3000, 1), (3.0, 4.0, 6000, 1)], gap=0.8)
  h, top, depth, rad = stone_cells(stones, 42, gap=1.2, peak=(0.35, 0.5), gamma=0.55,
                                   soft=0.8, bulge=1.1)
  stone = top >= 0

  # Distance to each crack's centre line (mm) and the ribbon's half width.

  def dist(f):
    gv, gu = np.gradient(f(U), 1 / S)
    return np.abs(V - f(U)) / np.sqrt(1 + gu * gu)

  half = 3.4 + 0.6 * wave_noise(U, V, 44, 14) + 0.35 * wave_noise(U, V, 45, 4)
  d_main = dist(crack_path)
  taper = np.clip((168 - U) / 64, 0, 1) * (U > 104)
  d_branch = np.where(taper > 0, dist(branch_path), np.inf)
  hw_branch = half * (0.35 + 0.65 * taper)
  q = np.minimum(d_main / half, d_branch / np.maximum(hw_branch, 1e-3))
  ribbon = q < 1
  core = np.minimum(d_main, d_branch)

  # The tar stands 0.9 mm proud, crowned, with a shallow groove where it sank into the crack.

  tar = 0.9 * np.clip(1 - q * q, 0, 1) ** 1.5 - 0.2 * np.exp(-(core / 0.8) ** 2)
  h = np.where(ribbon, tar, h)

  # The binder is matte and low, so it takes only a sparse stipple.

  stone &= ~ribbon
  t = np.where(stone, equalize(lambert(h, light), stone), 0.15)
  t[cast_shadow(h, light, 2.0)] = 0
  t = blur(t, 0.2)
  dots = stipple(t, 46, 1.75, 1.0, 0.8, 0.5, 0.88) & ~ribbon

  # The glint falls where the tar's normal comes nearest the half-way vector
  # between the light and the eye above: the brightest 38 % of the ribbon.

  hv, hu = np.gradient(h, 1 / S)
  half_vec = light + np.array([0, 0, 1.0])
  half_vec /= np.linalg.norm(half_vec)
  glint = (half_vec[2] - half_vec[0] * hu - half_vec[1] * hv) / np.sqrt(1 + hu * hu + hv * hv)
  shine = ribbon & (equalize(glint, ribbon) > 0.62)

  # The tar's edge is a dark rim at least 0.8 mm wide, so the ribbon reads as one piece.

  rim = (q < 1.0 + 0.8 / half) & ~ribbon
  return (dots & ~rim) | shine


# --- F4: worn asphalt with a polished wheel path ----------------------------

def f4_wheelpath():
  """Worn asphalt with a tyre-polished wheel path running across the face,
  48 mm wide, sunk into a shallow rut. Outside the path the surface has
  ravelled: stones stand proud and pits show where stones have gone. Inside
  it the stones are ground flat. Low raking light from the upper left,
  25 degrees up. Horizontal streaks 1.7 mm apart run with the traffic and
  ride up over each stone, swelling on lit faces and thinning in shadow; on
  the polished flat tops in the path they widen until only thin dark lines
  are left, so the tops shine."""
  light = light_dir(35, 25)
  stones = place_stones(51, [(2.2, 2.8, 400, -1), (4.0, 5.5, 3000, 1),
                             (2.8, 4.0, 9000, 1)], gap=0.3)
  h, top, depth, rad = stone_cells(stones, 52, gap=1.0, peak=(0.35, 0.55), gamma=0.7, bulge=1.2,
                                   squash=0.7)
  centre = 76 + 3 * np.sin(2 * math.pi * U / 170 + 0.8)
  wear = np.clip((26 - np.abs(V - centre)) / 8, 0, 1)
  wear = wear * wear * (3 - 2 * wear)

  # Ground flat to 0.3 mm in the path; the pits in it have filled with grime.

  cap = 0.3 + 4 * (1 - wear)
  polished = (h > cap) & (top >= 0) & (wear > 0.4)
  h = np.where(h < 0, h * (1 - wear), np.minimum(h, cap))
  h = h + 0.06 * wave_noise(U, V, 53, 1.6) * (1 - wear)
  rut = -1.6 * np.exp(-((V - centre) / 16) ** 2)
  h = h + rut

  # Outside the path the binder sits low between proud stones and stays
  # dark; in the path it has worn smooth and takes the light of the rut.

  t = equalize(lambert(h, light))
  t = np.where(top < 0, t * (0.25 + 0.75 * wear), t)
  t = np.where(polished, np.maximum(t, 0.85), t)
  t = blur(t, 0.15)
  t[cast_shadow(h, light, 3.0)] = 0
  dist, period = warped_lines(V + 0.5 * wave_noise(U, V, 54, 60), 2.0, h - rut, 0.6)
  return line_marks(dist, period, t, lo=0.12, hi=0.95)


# --- Output -------------------------------------------------------------------

CANDIDATES = [
  ('F0', f0_fresh,
   'fresh dense asphalt: crushed chips 2 to 5 mm across packed tight, binder seams 0.7 mm, '
   'pits 3 to 4 mm across, no repeat',
   'light from the upper left, 50 degrees up; each chip a faceted pyramid whose light-facing '
   'facets print as solid light shards, facets turned away and the binder dark; each pit shows '
   'a light crescent on its lower-right wall; chips cast dark shadows up to 2 mm toward the lower right'),
  ('F1', f1_open,
   'coarse open-graded asphalt: angular stones 6 to 14 mm across with chips 3 to 4 mm between, '
   'over deep voids 1.3 mm and wider, no repeat',
   'light from the upper left, 40 degrees up; contour rings 1.9 mm apart follow each stone '
   'inward, 0.9 to 1.2 mm wide, solid on the facets facing the light, thinning out on the far '
   'side; the outline ring holds; chips are solid light caps; voids and cast shadows (up to '
   '3 mm) solid dark'),
  ('F2', f2_alligator,
   'alligator-cracked asphalt: heaved slabs 6 to 18 mm across, each at its own tilt, split by '
   'crack valleys 0.8 to 2 mm wide, finer cracks in patches, no repeat',
   'low raking light from the left, 20 degrees round toward the top, 30 degrees up; each slab '
   'hatched along its own contour lines 1.9 mm apart, 0.9 to 1.2 mm wide; lips facing the '
   'light go solid, edges rolling into shadow thin to nothing; cracks and their cast shadows '
   '(up to 2.5 mm) dark'),
  ('F3', f3_sealed,
   'weathered asphalt: rounded stones 4 to 8 mm across in open binder, with a crack sealed by '
   'a 7 mm ribbon of glossy tar snaking across the face and one branch dying out, no repeat',
   'light from the top left, 65 degrees round from the left, 55 degrees up; aggregate '
   'stippled with 1.0 to 1.15 mm dots on a 1.9 mm blue-noise set, density following the '
   'light, solid on lit crowns, none in cast shadow; the tar is dark with a specular '
   'highlight stripe on its lit shoulder, split by the groove where it sank into the crack, '
   'and a 0.8 mm dark rim'),
  ('F4', f4_wheelpath,
   'worn asphalt: stones 2.6 to 6.4 mm across, ravelled with pits outside a 48 mm '
   'tyre-polished wheel path sunk 1.6 mm into a rut, stones in the path ground flat, no repeat',
   'low raking light from the upper left, 35 degrees round from the left, 25 degrees up; '
   'horizontal streaks 1.7 mm apart run with the traffic and ride up over each stone, '
   '0.9 to 1.0 mm on shaded faces, solid on lit ones; polished flat tops solid light in a '
   'dark ring; the rut wall under the light falls darker, the far wall lighter'),
]


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
