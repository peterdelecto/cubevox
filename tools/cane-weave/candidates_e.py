#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family E: five readings of a concrete sidewalk.

Each candidate models the sidewalk as a height field, lights it, and turns the
light into printable two-tone marks. Each candidate function returns the
light-inlay mask on the face grid of candidates_b.py: 10 px/mm, 1920 x 1520 px,
u right and v down in mm from the face's top-left corner. The face geometry,
the shared tools and the printability pass come from candidates_b.py.

  candidates_e.py           every candidate as <name>.png; prints the report lines
  candidates_e.py E0 E3     only the named candidates
"""

import math
import random
import sys
import time

import numpy as np
from PIL import Image

import candidates_b as cb
from candidates_b import (S, W, H, U, V, MIN_LIGHT, MIN_DARK, OUT, blur, equalize,
                          hash01, line_dist, line_marks, wave_noise)

# Slab layout, face grid mm. Joints run midway between hole columns, and the
# cross joint passes between the OLED window and the first hole row.

FIELD_UV = (3.0, 3.0, 189.0, 149.0)
JOINTS_U = np.array([48.0, 96.0, 144.0])
JOINTS_V = np.array([72.0])
EDGES_U = np.array([FIELD_UV[0], *JOINTS_U, FIELD_UV[2]])
EDGES_V = np.array([FIELD_UV[1], *JOINTS_V, FIELD_UV[3]])


# --- Shared tools ---------------------------------------------------------

def light(toward_u, toward_v, elev):
  """Unit vector toward a light that lies in the face direction
  (toward_u, toward_v), `elev` degrees above the face."""
  a, n = math.radians(elev), math.hypot(toward_u, toward_v)
  return (math.cos(a) * toward_u / n, math.cos(a) * toward_v / n, math.sin(a))


def shade(h, lit):
  """Cosine between the normal of height field h (mm) and the light vector."""
  hv, hu = np.gradient(h, 1 / S)
  lu, lv, lz = lit
  return np.clip((lz - lu * hu - lv * hv) / np.sqrt(1 + hu * hu + hv * hv), 0, 1)


def shifted(a, di, dj, fill):
  """out[j, i] = a[j + dj, i + di], `fill` where that falls off the grid."""
  out = np.full(a.shape, fill, a.dtype)
  h, w = a.shape
  out[max(0, -dj):min(h, h - dj), max(0, -di):min(w, w - di)] = \
      a[max(0, dj):min(h, h + dj), max(0, di):min(w, w + di)]
  return out


def cast(h, lit, reach):
  """Pixels whose ray toward the light meets the height field within `reach` mm."""
  lu, lv, lz = lit
  flat = math.hypot(lu, lv)
  out = np.zeros(h.shape, bool)
  seen = set()
  for k in range(1, int(reach * S) + 1):
    di, dj = round(k * lu / flat), round(k * lv / flat)
    if (di, dj) in seen:
      continue
    seen.add((di, dj))
    out |= shifted(h, di, dj, -np.inf) > h + math.hypot(di, dj) / S * lz / flat
  return out


def value_noise(x, key):
  """Smooth noise in [0, 1] along x, lattice step 1, one stream per integer key."""
  i = np.floor(x).astype(np.int64)
  f = x - i
  f = f * f * (3 - 2 * f)
  a, b = hash01(key, i), hash01(key, i + 1)
  return a + (b - a) * f


def poisson(r, seed):
  """Points over the face (mm) no closer than r mm, by Bridson's dart throwing."""
  rng = random.Random(seed)
  w, h = W / S, H / S
  cell = r / math.sqrt(2)
  grid = np.full((int(h / cell) + 1, int(w / cell) + 1), -1, np.int64)
  pts, active = [], []

  def add(u, v):
    grid[int(v / cell), int(u / cell)] = len(pts)
    active.append(len(pts))
    pts.append((u, v))

  add(rng.uniform(0, w), rng.uniform(0, h))
  while active:
    n = rng.randrange(len(active))
    pu, pv = pts[active[n]]
    for _ in range(20):
      a, d = rng.uniform(0, 2 * math.pi), rng.uniform(r, 2 * r)
      u, v = pu + d * math.cos(a), pv + d * math.sin(a)
      if not (0 <= u < w and 0 <= v < h):
        continue
      ci, cj = int(u / cell), int(v / cell)
      near = grid[max(0, cj - 2):cj + 3, max(0, ci - 2):ci + 3]
      if all((pts[k][0] - u) ** 2 + (pts[k][1] - v) ** 2 >= r * r for k in near[near >= 0]):
        add(u, v)
        break
    else:
      active[n] = active[-1]
      active.pop()
  return np.array(pts)


def stamp(points, dia):
  """Discs `dia` mm across (one value, or one per point) centred on points (mm)."""
  out = np.zeros((H, W), bool)
  for (pu, pv), d in zip(points, np.broadcast_to(dia, (len(points),))):
    r, ci, cj = d * S / 2, pu * S, pv * S
    i0, i1 = max(0, int(ci - r)), min(W, int(ci + r) + 2)
    j0, j1 = max(0, int(cj - r)), min(H, int(cj + r) + 2)
    if i0 < i1 and j0 < j1:
      ii, jj = np.arange(i0, i1) + 0.5, np.arange(j0, j1) + 0.5
      out[j0:j1, i0:i1] |= (ii[None, :] - ci) ** 2 + (jj[:, None] - cj) ** 2 <= r * r
  return out


def clear_of(points, mask, gap):
  """Which points (mm) have no pixel of `mask` within `gap` mm, judged on a square."""
  g = int(gap * S) + 1
  keep = np.ones(len(points), bool)
  for n, (pu, pv) in enumerate(points):
    i, j = int(pu * S), int(pv * S)
    keep[n] = not mask[max(0, j - g):j + g + 1, max(0, i - g):i + g + 1].any()
  return keep


def sample(a, points):
  """Values of grid array a at points (mm)."""
  j = np.clip((points[:, 1] * S).astype(int), 0, H - 1)
  i = np.clip((points[:, 0] * S).astype(int), 0, W - 1)
  return a[j, i]


def polyline_field(pts, widths, pad):
  """Distance (mm) from each pixel to the polyline through pts, and the line's
  width at its nearest point, over the polyline's box grown by pad mm.
  Returns both arrays and the box as a pair of slices into the face grid."""
  pts = np.asarray(pts, float)
  i0, i1 = max(0, int((pts[:, 0].min() - pad) * S)), min(W, int((pts[:, 0].max() + pad) * S) + 1)
  j0, j1 = max(0, int((pts[:, 1].min() - pad) * S)), min(H, int((pts[:, 1].max() + pad) * S) + 1)
  win = (slice(j0, max(j0, j1)), slice(i0, max(i0, i1)))
  dist, wid = np.full(U[win].shape, np.inf), np.zeros(U[win].shape)
  for (au, av), (bu, bv), wa, wb in zip(pts[:-1], pts[1:], widths[:-1], widths[1:]):
    si0 = max(i0, int((min(au, bu) - pad) * S))
    si1 = min(i1, int((max(au, bu) + pad) * S) + 1)
    sj0 = max(j0, int((min(av, bv) - pad) * S))
    sj1 = min(j1, int((max(av, bv) + pad) * S) + 1)
    if si0 >= si1 or sj0 >= sj1:
      continue
    sub = (slice(sj0 - j0, sj1 - j0), slice(si0 - i0, si1 - i0))
    uu, vv = U[sj0:sj1, si0:si1], V[sj0:sj1, si0:si1]
    eu, ev = bu - au, bv - av
    t = np.clip(((uu - au) * eu + (vv - av) * ev) / max(eu * eu + ev * ev, 1e-9), 0, 1)
    d = np.hypot(uu - au - t * eu, vv - av - t * ev)
    nearer = d < dist[sub]
    dist[sub] = np.where(nearer, d, dist[sub])
    wid[sub] = np.where(nearer, wa + t * (wb - wa), wid[sub])
  return dist, wid, win


def crack_path(rng, u, v, heading, length, box, step=0.9):
  """A jagged crack wandering about `heading` (radians) for up to `length` mm,
  stopping where it leaves box (u0, v0, u1, v1)."""
  pts, drift = [(u, v)], 0.0
  for _ in range(int(length / step)):
    drift = 0.85 * drift + rng.gauss(0, 0.22)
    a = heading + drift + rng.gauss(0, 0.4)
    u, v = u + step * math.cos(a), v + step * math.sin(a)
    if not (box[0] < u < box[2] and box[1] < v < box[3]):
      break
    pts.append((u, v))
  return pts


def crack_widths(rng, n, lo, hi):
  """Widths (mm) along a crack of n points: a random walk in [lo, hi] that
  narrows to lo at the far end."""
  w, out = rng.uniform(lo, hi), []
  for k in range(n):
    w = min(hi, max(lo, w + rng.gauss(0, 0.08)))
    out.append(lo + (w - lo) * min(1.0, 1.5 * (n - 1 - k) / max(n - 1, 1)))
  return out


def slabs():
  """Each pixel's slab column and row, and its signed distance (mm) to the
  nearest joint or field edge, positive on the lower or right side of it."""
  iu = np.searchsorted(JOINTS_U, U)
  iv = np.searchsorted(JOINTS_V, V)
  s = np.full(U.shape, np.inf)
  for line, coord in [(x, U) for x in EDGES_U] + [(y, V) for y in EDGES_V]:
    d = coord - line
    s = np.where(np.abs(d) < np.abs(s), d, s)
  return iu, iv, s


def slab_centres(iu, iv):
  return (EDGES_U[iu] + EDGES_U[iu + 1]) / 2, (EDGES_V[iv] + EDGES_V[iv + 1]) / 2


def fixture_distance():
  """Distance (mm) from each pixel to the nearest clearance ring or the OLED
  window's clearance edge, negative inside them."""
  d = np.full(U.shape, np.inf)
  for x, y, dia in cb.HOLES:
    hu, hv = cb.to_uv(x, y)
    d = np.minimum(d, np.hypot(U - hu, V - hv) - dia / 2 - cb.CLEAR)
  ou0, ov0 = cb.to_uv(cb.OLED[0], cb.OLED[3])
  ou1, ov1 = cb.to_uv(cb.OLED[2], cb.OLED[1])
  qu = np.maximum(ou0 - cb.CLEAR - U, U - ou1 - cb.CLEAR)
  qv = np.maximum(ov0 - cb.CLEAR - V, V - ov1 - cb.CLEAR)
  box = np.hypot(np.clip(qu, 0, None), np.clip(qv, 0, None)) + np.minimum(np.maximum(qu, qv), 0)
  return np.minimum(d, box)


# --- E0: broom finish -------------------------------------------------------

def e0_broom():
  """Broom-finished slabs 48 mm wide under raking light from the upper left,
  25 degrees up. Inside its tooled joints each slab has a smooth troweled
  margin 3 to 4.5 mm wide; broom drag lines about 2 mm apart cover the rest,
  bowed and wobbling, each slab broomed at its own angle. The drag lines vary
  in depth along their length and break off where the bristles skipped, so
  the shadow in each groove, drawn dark, widens where the groove is deep."""
  lit = light(-1, -1, 25)
  iu, iv, s = slabs()
  e = np.abs(s)

  # The tooled joint: a groove 1 mm wide, its edges rounded to 1.6 mm.

  h = np.where(e < 0.5, -1.4, np.sqrt(np.clip(2.56 - np.clip(2.1 - e, 0, None) ** 2, 0, None)) - 1.6)

  key = iu + 4 * iv
  cu, cv = slab_centres(iu, iv)
  x, y = U - cu, V - cv
  th = math.radians(5) * (hash01(key, 1) - 0.5)
  period = 1.75 + 0.35 * hash01(key, 2)
  bow = 3.0 * (hash01(key, 3) - 0.5)
  across = ((y * np.cos(th) - x * np.sin(th) + bow * (x / 24) ** 2
             + 0.35 * wave_noise(U, V, 3, 25)) / period + hash01(key, 4))
  n = np.floor(across).astype(np.int64)
  line = key * 100003 + n
  g = (across - n - 0.5 - 0.15 * (hash01(line, 5) - 0.5)) / 0.33
  depth = 0.25 + 0.4 * value_noise(U / 7.0, line)
  depth = depth * np.clip((value_noise(U / 9.0, line + 7) - 0.08) / 0.1, 0, 1)

  # Each drag line starts its own distance in from the joint, where the broom
  # touched down; the trowel also smoothed a collar round every knob.

  margin = 5.0 + 0.4 * wave_noise(U, V, 4, 40) + 1.2 * hash01(line, 6)
  collar = 1.6 + 0.6 * hash01(line, 7)
  depth = (depth * np.clip((e - margin) / 1.0, 0, 1)
           * np.clip((fixture_distance() - collar) / 1.0, 0, 1))
  h = h - depth * np.clip(1 - g * g, 0, None)

  # A thin dark line where the lit rounded edge of each joint meets the flat
  # margin keeps that edge's highlight apart from the margin.

  arris = (s > 2.1) & (s < 2.8)
  return ~(cast(h, lit, 3.0) | (shade(h, lit) < 0.3) | arris)


# --- E1: aged slabs -----------------------------------------------------------

def e1_aged():
  """Aged slabs 48 mm wide that have settled unevenly, light from the upper
  left and high, 45 degrees up and mostly from above. Each slab tilts its own
  way and sits a little higher or lower than its neighbours, so a raised edge
  throws a shadow across the expansion joint. Hairline cracks wander in from
  the joints, the surface is pocked with pits, and dark stipple dots shade
  the slab faces, denser as a face turns from the light and where grime
  gathers along the joints."""
  lit = light(-0.45, -1, 45)
  rng = random.Random(11)
  iu, iv, s = slabs()
  key = iu + 4 * iv
  cu, cv = slab_centres(iu, iv)
  # Each slab tilts and its corners curl up, so its face is a shallow tilted dish.

  a = 2 * math.pi * hash01(key, 11)
  tilt = 0.02 + 0.025 * hash01(key, 12)
  body = (1.0 * (hash01(key, 13) - 0.5)
          + tilt * ((U - cu) * np.cos(a) + (V - cv) * np.sin(a))
          + 0.0006 * ((U - cu) ** 2 + (V - cv) ** 2)
          + 0.12 * wave_noise(U, V, 12, 16))

  # Expansion joints 2 mm wide, their edges worn round and chipped.

  e = np.abs(s) - 0.6 * np.clip(wave_noise(U, V, 13, 3.5) - 0.6, 0, None)
  h = body - np.where(e < 1.0, 1.6, 0.7 * np.clip(1 - (e - 1.0) / 0.9, 0, 1) ** 2)

  # Hairline cracks start at a joint and stop at the next one; some fork.

  crack = np.zeros((H, W), bool)
  paths = []
  for col in range(4):
    for row in range(2):
      box = (EDGES_U[col] + 1.2, EDGES_V[row] + 1.2, EDGES_U[col + 1] - 1.2, EDGES_V[row + 1] - 1.2)
      for _ in range(rng.choice([0, 1, 1, 2])):
        side = rng.randrange(4)
        if side % 2 == 0:
          u = box[0] + 0.1 if side == 0 else box[2] - 0.1
          v = rng.uniform(box[1] + 8, box[3] - 8)
        else:
          u = rng.uniform(box[0] + 8, box[2] - 8)
          v = box[1] + 0.1 if side == 1 else box[3] - 0.1
        heading = (0.0, math.pi / 2, math.pi, -math.pi / 2)[side] + rng.uniform(-0.6, 0.6)
        pts = crack_path(rng, u, v, heading, rng.uniform(25, 70), box)
        paths.append((pts, crack_widths(rng, len(pts), 0.7, 1.25)))
        if len(pts) > 12 and rng.random() < 0.5:
          k = rng.randrange(len(pts) // 3, 2 * len(pts) // 3)
          fork = heading + rng.choice((-1, 1)) * rng.uniform(0.6, 1.1)
          sub = crack_path(rng, *pts[k], fork, rng.uniform(8, 25), box)
          paths.append((sub, crack_widths(rng, len(sub), 0.7, 0.9)))
  for pts, ws in paths:
    if len(pts) < 2:
      continue
    dist, wid, win = polyline_field(pts, ws, 2.0)
    h[win] -= 0.8 * np.clip(1 - dist / (wid / 2 + 0.5), 0, 1)
    crack[win] |= dist < wid / 2

  # Pits 0.9 to 2.5 mm across, gathered in patches.

  pits = poisson(2.4, 14)
  patch = wave_noise(pits[:, 0], pits[:, 1], 15, 30)
  pits = pits[hash01(np.arange(len(pits)), 16) < np.clip(0.18 + 0.2 * patch, 0.03, 0.55)]
  for (pu, pv), r in zip(pits, 0.45 + 0.8 * hash01(np.arange(len(pits)), 17) ** 2):
    i0, i1 = max(0, int((pu - r) * S)), min(W, int((pu + r) * S) + 2)
    j0, j1 = max(0, int((pv - r) * S)), min(H, int((pv + r) * S) + 2)
    d = np.hypot(U[j0:j1, i0:i1] - pu, V[j0:j1, i0:i1] - pv)
    h[j0:j1, i0:i1] -= 0.75 * r * np.clip(1 - (d / r) ** 2, 0, None)

  # Walls turned from the light and cast shadows are solid dark; walls facing
  # it (the far side of every crack, pit and raised edge) stay clean light.
  # Stipple carries the tone of each slab face, taken from the face alone so
  # cracks and pits do not smear into it, plus the grime that gathers along
  # the joints.

  t = shade(h, lit)
  dark = cast(h, lit, 3.0) | (t < 0.45) | crack
  bright = t > lit[2] + 0.04
  k = (0.05 + 0.6 * (1 - equalize(shade(body, lit), cb.ALLOWED))
       + 0.5 * np.exp(-np.clip(np.abs(s) - 1.5, 0, None) / 3.0)
       + 0.12 * wave_noise(U, V, 18, 20))
  dots = poisson(1.8, 19)
  kk = sample(k, dots)
  keep = ((hash01(np.arange(len(dots)), 20) < kk) & ~sample(bright, dots)
          & clear_of(dots, dark, 1.3))
  return ~dark & ~stamp(dots[keep], 0.9)


# --- E2: exposed aggregate ----------------------------------------------------

def e2_aggregate():
  """Exposed-aggregate concrete: rounded stones 2.5 to 10 mm across standing
  proud of the cement, packed at random, light from the upper left, 40
  degrees up. Each stone is lit solid on its upper-left side and turns dark
  toward the lower right; the larger ones keep a thin rim of light thrown up
  from the cement along their shadow edge. The stones shade the cement to
  their lower right, and the open cement shows a scatter of sand grains."""
  lit = light(-1, -1, 40)
  rng = random.Random(23)

  # Largest stones first, smaller ones into the gaps; 0.7 mm of cement at least.

  stones, buckets, cell = [], {}, 11.0
  for rmax in np.linspace(4.0, 1.2, 12):
    for _ in range(16000):
      r = rmax * rng.uniform(0.88, 1.0)
      lob = rng.uniform(0.04, 0.18)
      reach = r * (1 + lob + 0.07)
      u, v = rng.uniform(-5, W / S + 5), rng.uniform(-5, H / S + 5)
      bi, bj = int(u // cell), int(v // cell)
      if all((pu - u) ** 2 + (pv - v) ** 2 >= (pr + reach + 0.7) ** 2
             for i in (bi - 1, bi, bi + 1) for j in (bj - 1, bj, bj + 1)
             for pu, pv, pr in buckets.get((i, j), ())):
        stones.append((u, v, r, lob, rng.uniform(0, math.pi),
                       rng.uniform(0, 2 * math.pi), rng.uniform(0, 2 * math.pi)))
        buckets.setdefault((bi, bj), []).append((u, v, reach))

  hgt, edge, away = np.zeros((H, W)), np.zeros((H, W)), np.zeros((H, W))
  stone, big = np.zeros((H, W), bool), np.zeros((H, W), bool)
  au, av = -lit[0] / math.hypot(lit[0], lit[1]), -lit[1] / math.hypot(lit[0], lit[1])
  for cu, cv, r, lob, phi, p3, p5 in stones:
    reach = r * (1 + lob + 0.07)
    i0, i1 = max(0, int((cu - reach) * S)), min(W, int((cu + reach) * S) + 2)
    j0, j1 = max(0, int((cv - reach) * S)), min(H, int((cv + reach) * S) + 2)
    if i0 >= i1 or j0 >= j1:
      continue
    win = (slice(j0, j1), slice(i0, i1))
    dx, dy = U[win] - cu, V[win] - cv
    d, th = np.hypot(dx, dy), np.arctan2(dy, dx)
    rad = r * (1 + lob * np.cos(2 * (th - phi)) + 0.05 * np.cos(3 * th + p3)
               + 0.02 * np.cos(5 * th + p5))
    rho = d / rad
    m = rho < 1
    hgt[win] = np.where(m, 0.65 * r * np.clip(1 - rho * rho, 0, None) ** 0.55, hgt[win])
    edge[win] = np.where(m, (1 - rho) * rad, edge[win])
    away[win] = np.where(m, (dx * au + dy * av) / np.maximum(d, 1e-6), away[win])
    stone[win] |= m
    if r >= 2.8:
      big[win] |= m

  # The rim runs along the shadow edge of each large stone and stops 0.8 mm
  # short of the lit body, so a dark band always parts the two.

  shadow = cast(hgt, lit, 4.0)
  t = equalize(shade(hgt, lit), stone)
  body = stone & (t > 0.4) & ~shadow
  rim = big & (away > 0.2) & (edge < 0.95) & ~cb.dilate(body, cb.disc(16))
  sand = poisson(1.9, 29)
  sand = sand[(hash01(np.arange(len(sand)), 30) < 0.4) & clear_of(sand, stone | shadow, 1.45)]
  return body | rim | stamp(sand, 0.95)


# --- E3: cracked and lifted slab ----------------------------------------------

def e3_lifted():
  """One large slab cracked from top to bottom, its left piece heaved up by a
  root and tilted, light from the left and low, 30 degrees up. The heaved
  edge stands up to 1.5 mm proud and throws a shadow up to 2.6 mm wide across
  the crack, and a line of weeds grows out of the crack's lower half.
  Horizontal woodcut lines 2.6 mm apart lie on the surface as if seen a
  little from below, so they climb the tilted piece and jog at the step, and
  they swell or thin with the light."""
  lit = light(-1, -0.25, 30)
  rng = random.Random(31)

  # The crack's course, u as a function of v, wandering and jagged; it
  # passes between the OLED window and the hole columns.

  vs = np.arange(-4.0, 157.0, 1.1)
  us = (np.interp(vs, [-4, 20, 45, 62, 80, 100, 125, 157],
                  [111, 109, 108.5, 105, 98, 96, 95.5, 93])
        + 1.6 * np.sin(vs / 13 + 1) + 0.8 * np.sin(vs / 5.3 + 2)
        + np.array([rng.uniform(-0.45, 0.45) for _ in vs]))
  rise_at = lambda v: 0.35 + 2.1 * np.exp(-((v - 118) / 34) ** 2)
  at, rise = np.interp(V, vs, us), rise_at(V)
  left = U < at

  # The left piece hinges on the joint at u = 48 and rises to the crack; the
  # right piece lifts a quarter as much; the slab right of u = 144 has sunk.

  h = np.where(left & (U > 48), rise * np.clip((U - 48) / (at - 48), 0, 1) ** 1.2, 0.0)
  h = np.where(~left & (U < 144), 0.25 * rise * np.clip((144 - U) / (144 - at), 0, 1) ** 2, h)
  h = np.where(U >= 144, -0.35, h)

  ej = np.minimum(np.abs(U - 48), np.abs(U - 144)) + 0.15 * wave_noise(U, V, 35, 30)
  h = h - np.where(ej < 0.6, 1.4,
                   0.8 * (1 - np.sqrt(np.clip(1 - (np.clip(1.4 - ej, 0, None) / 0.8) ** 2, 0, None))))
  h = h + 0.12 * wave_noise(U, V, 32, 30) + 0.04 * wave_noise(U, V, 33, 10)

  widths = (0.9 + (rise_at(vs) - 0.35) / 1.6 + np.array([rng.uniform(-0.15, 0.25) for _ in vs]))
  dist, wid, win = polyline_field(list(zip(us, vs)), widths, 3.0)
  gap = np.zeros((H, W), bool)
  gap[win] = dist < wid / 2
  h[win] -= np.where(gap[win], 2.5, 0.5 * np.clip(1 - (dist - wid / 2) / 0.6, 0, 1))

  # Hairline cracks: a fork off the main crack into the heaved piece, one in
  # each of the side slabs.

  hair = np.zeros((H, W), bool)
  k = int(np.searchsorted(vs, 126))
  for u0, v0, heading, length in ((us[k], vs[k], 2.2, 45), (47.0, 30.0, 2.8, 40),
                                  (188.5, 128.0, 3.6, 30)):
    pts = crack_path(rng, u0, v0, heading, length, (0, 0, W / S, H / S))
    if len(pts) < 2:
      continue
    d, w, sub = polyline_field(pts, crack_widths(rng, len(pts), 0.75, 0.95), 2.0)
    h[sub] -= 0.6 * np.clip(1 - d / (w / 2 + 0.4), 0, 1)
    hair[sub] |= d < w / 2

  # Tone follows the light straight, so the big forms set it and the wear
  # only ripples it; a flat face sits at 0.45.

  tone = blur(np.clip(0.45 + 5.0 * (shade(h, lit) - lit[2]), 0, 1), 0.6)
  tone[cast(h, lit, 5.0) | gap | hair] = 0
  s = V + 1.1 * h + 0.3 * wave_noise(U, V, 34, 40)
  gv, gu = np.gradient(s, 1 / S)
  stretch = np.clip(np.hypot(gu, gv), 0.5, 2.0)
  light_mask = line_marks(line_dist(s, 2.4) / stretch, 2.4 / stretch, tone, lo=0.02, hi=0.95)

  # Weeds: four tufts of tapering blades rising out of the crack between the
  # hole rows, bigger near the root. Each blade clears a dark margin round
  # itself and throws a shadow down and to the right, so blades in front
  # cross those behind.

  blades = []
  for vb, size in ((76.0, 0.75), (100.0, 1.0), (125.0, 0.95), (147.0, 0.8)):
    ub = float(np.interp(vb, vs, us))
    for _ in range(rng.randint(7, 11)):
      a = -math.pi / 2 + rng.uniform(-0.85, 0.85)
      bend = math.copysign(rng.uniform(0.02, 0.06), a + math.pi / 2)
      n = max(4, int(size * rng.uniform(7, 16) / 0.8))
      u, v = ub + rng.uniform(-0.8, 0.8), vb + rng.uniform(-1.5, 1.5)
      w0 = 1.3 + 0.6 * size
      pts, ws = [(u, v)], [w0]
      for j in range(1, n + 1):
        a += bend * 0.8
        u, v = u + 0.8 * math.cos(a), v + 0.8 * math.sin(a)
        pts.append((u, v))
        ws.append(max(0.95, w0 * (1 - 0.6 * j / n)))
      blades.append((vb + rng.random(), pts, ws))
  fl = math.hypot(lit[0], lit[1])
  di, dj = round(12 * lit[0] / fl), round(12 * lit[1] / fl)
  for _, pts, ws in sorted(blades):
    d, w, sub = polyline_field(pts, ws, 3.0)
    halo = d < w / 2 + 0.75
    light_mask[sub] &= ~halo & ~shifted(halo, di, dj, False)
    light_mask[sub] |= d < w / 2
  return light_mask


# --- E4: old paving flags -----------------------------------------------------

E4_COURSES = (-12.0, 25.0, 71.0, 99.6, 125.0, 162.0)


def e4_flags():
  """Old stone paving flags laid in courses 22 to 46 mm deep, 24 to 72 mm
  long, their edges worn round, faces dished by feet, rippled where the
  stone was split, light from the upper left, 50 degrees up. Engraved dark
  cross-hatching on light stone carries the shade: one set of diagonal lines
  swells as the face turns from the light, a second set crosses it on the
  worn lower-right edges, and the lines bow over the form. Joints between
  flags are 1.4 to 2.1 mm wide; vertical joints fall midway between hole columns."""
  lit = light(-1, -1, 50)
  rng = random.Random(41)
  e = np.full((H, W), -1.0)
  h = np.full((H, W), -1.6)
  prev, seed = set(), 100
  for va, vb in zip(E4_COURSES[:-1], E4_COURSES[1:]):
    u, joints = -24.0 * rng.randint(0, 2), set()
    while u < W / S:
      nxt = u + 24 * rng.choice([1, 2, 2, 3] if vb - va < 32 else [2, 2, 3])
      while nxt in prev:
        nxt += 24
      gj = 0.7 + 0.35 * rng.random()
      ua, ub = u + gj + rng.uniform(-0.3, 0.3), nxt - gj + rng.uniform(-0.3, 0.3)
      fa, fb = va + gj + rng.uniform(-0.3, 0.3), vb - gj + rng.uniform(-0.3, 0.3)
      joints.add(nxt)
      u = nxt
      i0, i1 = max(0, int(ua * S)), min(W, int(ub * S) + 1)
      j0, j1 = max(0, int(fa * S)), min(H, int(fb * S) + 1)
      if i0 >= i1 or j0 >= j1:
        continue
      win = (slice(j0, j1), slice(i0, i1))
      uu, vv = U[win], V[win]
      cu, cv, hu, hv = (ua + ub) / 2, (fa + fb) / 2, (ub - ua) / 2, (fb - fa) / 2
      rc, wear = rng.uniform(1.5, 5.0), rng.uniform(3.0, 4.5)
      qx, qy = np.abs(uu - cu) - (hu - rc), np.abs(vv - cv) - (hv - rc)
      inside = (rc - np.hypot(np.clip(qx, 0, None), np.clip(qy, 0, None))
                - np.minimum(np.maximum(qx, qy), 0))

      # Worn edges only ever recede, so a joint never closes.

      inside = (inside - 0.45 * np.clip(wave_noise(uu, vv, seed, 9) + 0.3, 0, None)
                - 0.15 * np.clip(wave_noise(uu, vv, seed + 1, 3), 0, None))
      x = np.clip(inside / wear, 0, 1)
      ra = rng.uniform(0, math.pi)
      face = (-1.1 * (1 - x) ** 2
              + rng.uniform(-0.012, 0.012) * (uu - cu) + rng.uniform(-0.012, 0.012) * (vv - cv)
              - 0.4 * np.exp(-((uu - cu) / (0.6 * hu)) ** 2 - ((vv - cv) / (0.6 * hv)) ** 2)
              + 0.15 * np.sin(2 * math.pi * ((uu - cu) * math.cos(ra) + (vv - cv) * math.sin(ra))
                             / rng.uniform(5, 9) + 1.5 * wave_noise(uu, vv, seed + 2, 20)))
      m = inside > 0
      e[win] = np.where(m, inside, e[win])
      h[win] = np.where(m, face, h[win])
      seed += 3
    prev = joints

  # Darkness follows the light straight: a flat face sits at 0.3, below the
  # first hatch, so only the dish, the ripples and the worn edges take lines.

  flag = e > 0
  k = blur(np.clip(0.3 + 2.5 * (lit[2] - shade(h, lit)), 0, 1), 0.5)
  k[cast(h, lit, 3.0)] = 1
  dark = ~flag | (k > 0.96)
  for n, (coord, k0, k1) in enumerate((((U + V) / math.sqrt(2), 0.4, 0.78),
                                       ((U - V) / math.sqrt(2), 0.68, 0.94))):
    a = coord + 0.45 * h + 0.25 * wave_noise(U, V, 44 + n, 30)
    gv, gu = np.gradient(a, 1 / S)
    g = np.hypot(gu, gv)
    w = MIN_DARK + (2.2 / g - MIN_LIGHT - MIN_DARK) * np.clip((k - k0) / (k1 - k0), 0, 1)
    dark |= (k > k0) & (line_dist(a, 2.2) / g < w / 2)
  return ~dark


# --- Output -------------------------------------------------------------------

CANDIDATES = [
  ('E0', e0_broom, '', ''),
  ('E1', e1_aged, '', ''),
  ('E2', e2_aggregate, '', ''),
  ('E3', e3_lifted, '', ''),
  ('E4', e4_flags, '', ''),
]


def render(name, fn):
  t0 = time.time()
  light_mask, changed = cb.printable(fn())
  img = cb.face_image(light_mask)
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
