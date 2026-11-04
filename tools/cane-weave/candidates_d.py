#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family D: five readings of manilla folder card.

Each candidate models a height field of the card, lights it, and turns the light
into printable two-tone marks, using the face grid, the helpers and the
printability pass of candidates_b.py. Each candidate function returns the
light-inlay mask on the face grid (10 px/mm, 1920 x 1520 px, row 0 at the top
edge of the face).

  candidates_d.py           every candidate as <name>.png; prints the report lines
  candidates_d.py D0 D3     only the named candidates
"""

import math
import random
import sys

import numpy as np
from PIL import Image

from candidates_b import (H, HOLES, MIN_DARK, MIN_LIGHT, OUT, S, U, V, W, blur, equalize,
                          hash01, line_dist, line_marks, render, to_uv, wave_noise)


# --- Shared tools -------------------------------------------------------------

def light_vec(az, el):
  """Unit vector toward the light: az degrees round from the left edge toward
  the top edge (45 is the upper left), el degrees above the face."""
  a, e = math.radians(az), math.radians(el)
  return -math.cos(a) * math.cos(e), -math.sin(a) * math.cos(e), math.sin(e)


def shade(h, az, el):
  """Cosine between the surface normal of height field h (mm) and the light;
  a flat face gives sin(el)."""
  hv, hu = np.gradient(h, 1 / S)
  lu, lv, lz = light_vec(az, el)
  return np.clip((lz - lu * hu - lv * hv) / np.sqrt(1 + hu * hu + hv * hv), 0, 1)


def shadow(h, az, el, reach):
  """Pixels whose ray toward the light meets the height field within `reach` mm."""
  a = math.radians(az)
  slope = math.tan(math.radians(el))
  n = int(reach * S)
  hp = np.pad(h, n + 1, mode='edge')
  out = np.zeros(h.shape, bool)
  done = set()
  for k in range(1, n + 1):
    di, dj = round(-k * math.cos(a)), round(-k * math.sin(a))
    if (di, dj) in done:
      continue
    done.add((di, dj))
    src = hp[n + 1 + dj:n + 1 + dj + H, n + 1 + di:n + 1 + di + W]
    out |= src > h + math.hypot(di, dj) / S * slope
  return out


def grain(seed, radius):
  """Paper grain: pixel noise blurred to about `radius` mm, unit spread."""
  j, i = np.mgrid[0:H, 0:W]
  g = blur(hash01(i, j, seed) - 0.5, radius)
  return g / g.std()


def smooth(x):
  x = np.clip(x, 0, 1)
  return x * x * (3 - 2 * x)


def ridge(d, hw, wall):
  """0 to 1 across a ridge centred on d = 0: a flat crown, then a smooth wall
  `wall` mm wide down to zero at |d| = hw."""
  return smooth((hw - np.abs(d)) / wall)


def round_rect(u0, v0, u1, v1, r):
  """Signed distance, mm, to a rectangle with corner radius r; negative inside."""
  qx = np.abs(U - (u0 + u1) / 2) - ((u1 - u0) / 2 - r)
  qy = np.abs(V - (v0 + v1) / 2) - ((v1 - v0) / 2 - r)
  return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


def bowed_lines(s, period, t, lo, hi):
  """Line marks along the level lines of s, kept `period` mm apart on the face
  however s is warped."""
  gv, gu = np.gradient(s, 1 / S)
  stretch = np.maximum(np.hypot(gu, gv), 0.2)
  return line_marks(line_dist(s, period) / stretch, period / stretch, t, lo=lo, hi=hi)


# --- D0: the card's fibres, close up ------------------------------------------

def d0_fibres():
  """The card's surface seen as if through a lens: a felt of fibres 3.6 to 6 mm
  wide and 12 to 34 mm long, most running near the machine direction (left to
  right), lying over and under each other, with flecks and pits. Each fibre is
  a tube lit from the upper left and engraved with one or two strokes along its
  length that swell on the lit side and fade into its shadow; fibres on top cast
  shadows on the ones beneath."""
  rng = random.Random(5)
  floor = -0.6
  hgt = np.full(U.shape, floor)
  across = np.zeros(U.shape)      # signed mm from the top fibre's axis
  half = np.ones(U.shape)         # that fibre's half width here, mm
  slope = np.zeros(U.shape)       # its tube's fall, mm per mm, away from the axis
  out_u, out_v = np.zeros(U.shape), np.zeros(U.shape)   # unit vector away from the axis
  for _ in range(560):
    length = rng.uniform(16, 44)
    r = rng.uniform(1.8, 3.0)
    a = rng.gauss(0, 0.4) if rng.random() < 0.65 else rng.uniform(0, math.pi)
    cu, cv = rng.uniform(-10, W / S + 10), rng.uniform(-10, H / S + 10)
    bend = rng.uniform(-0.25, 0.25) * length
    ca, sa = math.cos(a), math.sin(a)
    ts = np.linspace(0, 1, 11)
    pu = ((1 - ts) ** 2 * (cu - ca * length / 2) + 2 * (1 - ts) * ts * (cu - sa * bend)
          + ts ** 2 * (cu + ca * length / 2))
    pv = ((1 - ts) ** 2 * (cv - sa * length / 2) + 2 * (1 - ts) * ts * (cv + ca * bend)
          + ts ** 2 * (cv + sa * length / 2))
    i0, i1 = max(0, int((pu.min() - r - 1) * S)), min(W, int((pu.max() + r + 1) * S) + 1)
    j0, j1 = max(0, int((pv.min() - r - 1) * S)), min(H, int((pv.max() + r + 1) * S) + 1)
    if i0 >= i1 or j0 >= j1:
      continue
    uu, vv = U[j0:j1, i0:i1], V[j0:j1, i0:i1]
    seg = np.hypot(np.diff(pu), np.diff(pv))
    start = np.concatenate([[0], np.cumsum(seg)])
    best = np.full(uu.shape, np.inf)
    side = np.zeros(uu.shape)
    pos = np.zeros(uu.shape)
    ou, ov = np.zeros(uu.shape), np.zeros(uu.shape)
    for k in range(len(seg)):
      du, dv = pu[k + 1] - pu[k], pv[k + 1] - pv[k]
      q = np.clip(((uu - pu[k]) * du + (vv - pv[k]) * dv) / (seg[k] ** 2), 0, 1)
      eu, ev = uu - pu[k] - q * du, vv - pv[k] - q * dv
      d = np.hypot(eu, ev)
      near = d < best
      best = np.where(near, d, best)
      side = np.where(near, np.sign(du * ev - dv * eu), side)
      pos = np.where(near, (start[k] + q * seg[k]) / start[-1], pos)
      ou = np.where(near, eu / np.maximum(d, 1e-6), ou)
      ov = np.where(near, ev / np.maximum(d, 1e-6), ov)

    # Fibres narrow toward their ends. Each new fibre lies on top of whatever
    # it crosses, so it is painted over the ones beneath.

    rr = r * (0.45 + 0.55 * np.clip(np.minimum(pos, 1 - pos) / 0.18, 0, 1) ** 0.5)
    inside = best < rr
    if not inside.any():
      continue
    core = best < 0.5 * rr
    base = max(0.0, hgt[j0:j1, i0:i1][core].max()) if core.any() else 0.0
    rise = np.sqrt(np.clip(rr * rr - best * best, 0.01, None))
    sub = (slice(j0, j1), slice(i0, i1))
    hgt[sub] = np.where(inside, base + 0.85 * rise, hgt[sub])
    across[sub] = np.where(inside, side * best, across[sub])
    half[sub] = np.where(inside, rr, half[sub])
    slope[sub] = np.where(inside, np.minimum(0.85 * best / rise, 4.0), slope[sub])
    out_u[sub] = np.where(inside, ou, out_u[sub])
    out_v[sub] = np.where(inside, ov, out_v[sub])

  # Each fibre is lit as its own tube; the stack's height sets the cast shadows.

  fibre = hgt > floor
  lu, lv, lz = light_vec(45, 40)
  lit = (lz + slope * (lu * out_u + lv * out_v)) / np.sqrt(1 + slope * slope)
  t = equalize(lit, fibre)
  t[shadow(hgt, 45, 40, 1.5)] = 0

  # A 0.7 mm dark rim outlines each fibre; inside it, as many strokes as fit
  # 1.7 mm apart run along the fibre.

  body = half - 0.7
  period = np.maximum(2 * body / np.maximum(1, np.floor(2 * body / 1.7)), 1.6)
  dist = line_dist(across + body, period, period / 2)
  light = line_marks(dist, period, t, lo=0.2, hi=0.75) & fibre & (np.abs(across) < body)
  return d0_flecks(light, fibre, rng)


def d0_flecks(light, fibre, rng):
  """Raised flecks (a light crown, a dark moat that widens to the lower right)
  and pits (dark, with their lower-right wall catching the light)."""
  js, is_ = np.nonzero(fibre)
  for _ in range(70):
    n = rng.randrange(len(js))
    cu, cv = (is_[n] + 0.5) / S, (js[n] + 0.5) / S
    i0, i1 = max(0, is_[n] - 30), min(W, is_[n] + 30)
    j0, j1 = max(0, js[n] - 30), min(H, js[n] + 30)
    uu, vv = U[j0:j1, i0:i1] - cu, V[j0:j1, i0:i1] - cv
    sub = light[j0:j1, i0:i1]
    if rng.random() < 0.6:
      r = rng.uniform(0.6, 0.9)
      sub &= np.hypot(uu - 0.25, vv - 0.25) >= r + 1.0
      sub |= np.hypot(uu, vv) < r
    else:
      r = rng.uniform(1.1, 1.6)
      d = np.hypot(uu, vv)
      sub &= d >= r + 0.7
      sub |= (d < r) & (np.hypot(uu + 0.75, vv + 0.75) >= r)
  return light


# --- D1: a worn folder, soft creases and a bent corner --------------------------

D1_CREASES = [
  # (u0, v0, u1, v1) mm, half width mm, height mm (+ ridge, - valley)
  ((-10, 38, 202, 98), 10.0, -2.2),
  ((66, -10, 104, 162), 13.0, 2.0),
  ((118, -10, 202, 66), 7.0, -1.4),
  ((-10, 118, 96, 162), 8.0, 1.4),
  ((128, 84, 196, 152), 6.0, -1.1),
  ((18, -4, 44, 62), 5.0, 1.0),
]


def d1_creases():
  """A worn folder: the card undulates, six soft creases (ridges and valleys,
  10 to 26 mm wide, each with a crisp fold along its middle) run across it and
  die out, and the lower-right corner is bent up toward the viewer. Raking
  light from the left, 25 degrees up. Lines rising 20 degrees to the right ride
  over the relief, so they bow along the creases and kink at the bent corner;
  their width follows the light and they never go solid."""
  h = 0.9 * wave_noise(U, V, 201, 90) + 0.3 * wave_noise(U, V, 202, 30)
  for n, ((u0, v0, u1, v1), hw, amp) in enumerate(D1_CREASES):
    length = math.hypot(u1 - u0, v1 - v0)
    cu, cv = (u1 - u0) / length, (v1 - v0) / length
    along = (U - u0) * cu + (V - v0) * cv
    off = -(U - u0) * cv + (V - v0) * cu + 1.5 * wave_noise(U, V, 210 + n, 50)
    env = smooth(np.minimum(along, length - along) / 25)
    h += amp * env * np.clip(1 - np.abs(off) / hw, 0, 1) ** 1.7

  # The corner bends up along a line from (146, 149) to (189, 108), with a
  # bend about 1.5 mm round.

  d = ((U - 146) * 41 + (V - 149) * 43) / math.hypot(41, 43)
  h += 0.5 * 0.5 * (d + np.sqrt(d * d + 2.25))

  # The flat card sits at 0.55, so a crease flank turned from the light thins
  # its lines rather than losing them.

  t = 0.55 + 1.6 * (shade(h, 15, 25) - math.sin(math.radians(25)))
  t[shadow(h, 15, 25, 4.0)] = 0
  t = blur(t, 0.3)
  s = U * math.sin(math.radians(20)) + V * math.cos(math.radians(20)) - 1.5 * h
  s += 0.2 * wave_noise(U, V, 220, 25)
  return bowed_lines(s, 2.2, t, lo=0.1, hi=None)


# --- D2: the scored fold, the tab edge and the curl -----------------------------

def d2_fold():
  """A folder lying on its side: the fold on the left with three expansion
  scores 6 mm apart beside it; the front panel's free edge, with a thumb notch,
  curls up as it nears the edge and casts a 3 to 4 mm shadow onto the back
  panel; the back panel's tab sticks out at the upper right, and the desk
  beyond is dark. Light from 30 degrees round from the left, 35 degrees up.
  Horizontal lines cross the scores and ride up over the curl; they swell to
  solid on the lit walls and break off in the grooves' shadow."""
  edge = 118 + 0.8 * wave_noise(U, V, 301, 70)
  front = (U < edge) & (np.hypot(U - edge, V - 76) >= 8.5)
  tab = 13 * smooth(np.minimum(V - 20, 60 - V) / 4)
  back = U < 175 + 0.5 * wave_noise(U, V, 302, 60) + tab

  # The card rolls over at the fold, at the field's left edge (u = 3 mm).

  x = np.clip((8 - U) / 5, 0, 1)
  h = 0.3 - 1.6 * (1 - np.sqrt(1 - x * x))
  for n, su in enumerate((13.0, 19.0, 25.0)):
    d = np.abs(U - su - 0.3 * wave_noise(U, V, 310 + n, 40))
    h += -0.7 * np.clip(1 - (d / 1.4) ** 2, 0, 1) ** 2 + 0.15 * np.exp(-((d - 1.9) / 0.6) ** 2)

  # The panel sags a little, and the curl rises over its last 26 mm, more
  # toward the top and bottom corners.

  h += 0.5 * wave_noise(U, V, 305, 110)
  corner = 1 + 0.5 * ((V - 76) / 73) ** 2
  h += 3.6 * corner * np.clip((U - (edge - 26)) / 26, 0, 1) ** 3
  h = np.where(front, h, np.where(back, 0.3 * wave_noise(U, V, 306, 90), -0.6))

  lit = shade(h, 30, 35)
  t = 0.5 + 1.8 * (lit - math.sin(math.radians(35))) + 0.05 * grain(303, 0.6)
  t[shadow(h, 30, 35, 6.0)] = 0
  s = V - 1.6 * blur(h, 0.5) + 0.25 * wave_noise(U, V, 304, 25)
  return bowed_lines(s, 2.2, t, lo=0.12, hi=0.92) & (front | back)


# --- D3: blind-embossed stamping ------------------------------------------------

def d3_emboss():
  """Blind stamping, no lettering: a raised seal with a scalloped rim, a ring
  channel, twenty rays and a central boss; a debossed label panel at the top
  left; three raised rules across the middle; a raised ring round every hole.
  Light from 60 degrees round from the left, 45 degrees up. The flat card is
  light, pierced by a stipple of dark pits on a jittered 2.6 mm grid; walls
  facing the light lose their pits and go solid light, walls turned away and
  the cast shadows go solid dark, and gently shaded slopes break into light
  dots."""
  h = np.zeros(U.shape)

  # Seal centred at (150, 27) mm, 17 mm to the rim, every wall 1.5 mm wide.

  du, dv = U - 150, V - 27
  rr, ang = np.hypot(du, dv), np.arctan2(dv, du)
  h += 0.8 * smooth((17 + 0.9 * np.cos(24 * ang) - rr) / 1.5)
  h -= 0.6 * ridge(rr - 13.2, 1.6, 1.2)
  h += 0.7 * ridge(rr - 8.8, 1.8, 1.2)
  h += 1.2 * np.sqrt(np.clip(1 - (rr / 4.6) ** 2, 0, 1))

  # Label panel pressed in, its walls 1.6 mm wide.

  h -= 0.8 * smooth(-round_rect(8, 6, 56, 25, 3) / 1.6)

  for vc in (63.0, 69.0, 75.0):
    h += 0.8 * ridge(V - vc - 0.3 * wave_noise(U, V, 330 + int(vc), 60), 2.0, 1.5)
  for x, y, dia in HOLES:
    hu, hv = to_uv(x, y)
    h += 0.8 * ridge(np.hypot(U - hu, V - hv) - (dia / 2 + 3.8), 1.8, 1.4)

  lit = shade(h, 60, 45)
  t = 0.56 + 1.5 * (lit - math.sin(math.radians(45))) + 0.05 * grain(331, 0.8)
  t[shadow(h, 60, 45, 2.0)] = 0

  # A pit that touches a shadow merges into it as a drip, so a 1.2 mm band
  # beside every shadow stays clear of pits.

  dark = t <= 0.1
  near = blur(dark.astype(float), 0.4) > 0.02
  t = np.where(near & ~dark & (t >= 0.5), 1.0, t)
  return stipple(t, 2.6, 30, 332, lo=0.1, split=0.5, hi=0.88, jitter=0.1)


def stipple(t, pitch, angle, seed, lo, split, hi, jitter):
  """Light dots on a square grid `pitch` mm apart, turned `angle` degrees, each
  dot moved up to `jitter` mm. The dots grow from MIN_LIGHT as t rises to
  `split`; above it the face goes light, pierced by dark pits midway between
  the dots that shrink to MIN_DARK as t rises to `hi`; above that, solid light."""
  a = math.radians(angle)
  x = (U * math.cos(a) + V * math.sin(a)) / pitch
  y = (-U * math.sin(a) + V * math.cos(a)) / pitch
  ix, iy = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64)
  j = jitter / pitch
  dot = np.full(U.shape, np.inf)
  pit = np.full(U.shape, np.inf)
  for ox in (-1, 0, 1):
    for oy in (-1, 0, 1):
      cx, cy = ix + ox, iy + oy
      sx = cx + 0.5 + j * (2 * hash01(cx, cy, seed) - 1)
      sy = cy + 0.5 + j * (2 * hash01(cx, cy, seed + 1) - 1)
      dot = np.minimum(dot, np.hypot(x - sx, y - sy) * pitch)
      px = cx + j * (2 * hash01(cx, cy, seed + 2) - 1)
      py = cy + j * (2 * hash01(cx, cy, seed + 3) - 1)
      pit = np.minimum(pit, np.hypot(x - px, y - py) * pitch)
  span = pitch - MIN_DARK - 2 * jitter - MIN_LIGHT
  dot_d = MIN_LIGHT + np.clip((t - lo) / (split - lo), 0, 1) * span
  pit_d = pitch - MIN_LIGHT - 2 * jitter - np.clip((t - split) / (hi - split), 0, 1) * (
    pitch - MIN_LIGHT - 2 * jitter - MIN_DARK)
  return np.where(t < split, (t > lo) & (dot < dot_d / 2),
                  (t >= hi) | (pit >= pit_d / 2))


# --- D4: a sun-faded folder, scuffs and a dog-ear --------------------------------

# Scuff centres (u, v) mm and scrape direction, degrees below the horizontal:
# near -45, across the light, so the scratch walls catch it.

D4_SCUFFS = [(30, 64, -30), (100, 78, -55), (150, 66, -40), (128, 124, -65), (46, 128, -20)]


def d4_faded():
  """A folder left in the sun: its upper part, which lay uncovered, has faded
  lighter up to a soft edge where another folder covered it; five scuffed
  patches carry clusters of short scratches; the top-right corner is folded
  over into a dog-ear whose flap shows the unfaded inside of the card and
  casts a long shadow. Low sun from the upper left, 22 degrees up. White-line
  cross-hatching carries the tone: lines rising to the right swell first, then
  lines falling to the right join them, then the light goes solid."""
  rng = random.Random(41)
  h = 0.5 * wave_noise(U, V, 401, 70)
  fuzz = np.zeros(U.shape)
  for n, (cu, cv, deg) in enumerate(D4_SCUFFS):
    fuzz = np.maximum(fuzz, smooth(1 - np.hypot(U - cu, V - cv) / 13))
    for _ in range(rng.randint(6, 10)):
      a = math.radians(deg + rng.gauss(0, 8))
      along, off = rng.gauss(0, 6), rng.gauss(0, 5)
      mu = cu + along * math.cos(a) - off * math.sin(a)
      mv = cv + along * math.sin(a) + off * math.cos(a)
      half_len, hw = rng.uniform(2, 7), rng.uniform(1.1, 1.4)
      q = np.clip((U - mu) * math.cos(a) + (V - mv) * math.sin(a), -half_len, half_len)
      d = np.hypot(U - mu - q * math.cos(a), V - mv - q * math.sin(a))
      h -= 0.6 * np.clip(1 - (d / hw) ** 2, 0, 1) ** 2

  # Dog-ear: the corner folds over along u - v = 160 mm; the flap lies on the
  # face with its tip at (163, 29) and lifts away from the fold.

  fold = (160 - (U - V)) / math.sqrt(2)
  cut = fold < 0
  flap = (U >= 163) & (V <= 29) & ~cut
  h = np.where(flap, 0.4 + 0.012 * fold * fold + 0.5 * np.exp(-(fold / 1.2) ** 2), h)
  h = np.where(cut, -0.3, h)

  faded = smooth((50 + 0.15 * (U - 96) + 2 * wave_noise(U, V, 402, 80) - V) / 3)
  lit = shade(h, 45, 22)
  t = (0.3 + 1.4 * (lit - math.sin(math.radians(22))) + 0.28 * faded * ~flap
       + 0.12 * fuzz + 0.05 * grain(403, 0.6))
  t[shadow(h, 45, 22, 10.0)] = 0
  t[cut] = 0

  wob = 0.25 * wave_noise(U, V, 404, 30)
  sa = (U - V) / math.sqrt(2) - 0.6 * h + wob
  sb = (U + V) / math.sqrt(2) - 0.6 * h - wob
  light = cross_hatch(sa, sb, 2.4, 2.6, t, lo=0.12, mid=0.5, hi=0.9)

  # The flap's left edge catches the light: a 1 mm light rim inside it, a
  # 0.7 mm dark line outside it.

  rim = flap & (U < 164) & (fold > 1.5)
  return (light & ~((U >= 162.3) & (U < 163) & (V <= 29))) | rim


def cross_hatch(sa, sb, pa, pb, t, lo, mid, hi):
  """Lines along the level lines of sa, `pa` mm apart, swell from MIN_LIGHT to
  full as t rises from lo to mid; lines of sb, `pb` mm apart, join them from mid
  and go solid at hi."""
  la = line_marks(line_dist(sa, pa), pa, (t - lo) / (mid - lo), lo=0.0, hi=None)
  lb = line_marks(line_dist(sb, pb), pb, (t - mid) / (hi - mid), lo=0.0, hi=1.0)
  return la | lb


# --- Output -------------------------------------------------------------------

CANDIDATES = [
  ('D0', d0_fibres,
   'the card\'s fibres close up, as if through a lens: a felt of fibres 3.6 to 6 mm wide and '
   '12 to 34 mm long, most near left to right, over and under each other, with 1.2 to 1.8 mm '
   'raised flecks and 2.2 to 3.2 mm pits, no repeat',
   'light from the upper left, 40 degrees up; each fibre a tube with a 0.7 mm dark rim and one or '
   'two strokes along its length, 1.6 to 2.4 mm apart, 0.9 mm wide on the shaded side up to solid '
   'on the lit crown; upper fibres cast solid shadows up to 3 mm onto the ones beneath; flecks '
   'are light crowns in a dark moat 0.65 mm wide at the upper left and 1.35 mm at the lower '
   'right; pits are dark with a light crescent up to 1 mm wide on the lower-right wall'),
  ('D1', d1_creases,
   'a worn folder: the card undulates, six soft creases 10 to 26 mm wide with a crisp fold down '
   'each middle run across and die out, and the lower-right corner bends up along a line from '
   'the bottom edge 43 mm in to the right edge 41 mm up, no repeat',
   'raking light from the left, 15 degrees round toward the top and 25 degrees up; lines 2.2 mm '
   'apart rising 20 degrees to the right ride over the relief by 1.3 times its height, so they '
   'bow along the creases and kink at the bend; widths run 0.9 to 1.5 mm with the light and '
   'never go solid; ridges cast solid shadows up to 6 mm toward the right'),
  ('D2', d2_fold,
   'a folder lying on its side: the rolled fold at the left edge with three expansion scores '
   '6 mm apart beside it, the front panel\'s free edge at about 118 mm with a 17 mm thumb '
   'notch, curling up to 2.4 to 3.8 mm over its last 30 mm, the back panel beyond with a 13 mm '
   'tab at the upper right, dark desk past it, no repeat',
   'light 30 degrees round from the left, 35 degrees up; horizontal lines 2.2 mm apart cross '
   'the scores and rise over the curl by 0.8 times its height; widths 0.9 to 1.5 mm with the '
   'light, solid on the lit groove walls and the rolled fold, gone in the grooves\' shadowed '
   'walls; the curled edge casts a solid shadow 3 to 5 mm wide onto the back panel'),
  ('D3', d3_emboss,
   'blind stamping with no lettering: a 32 mm raised seal (scalloped rim, ring channel, twenty '
   'rays, central boss) at the upper right, a 48 x 18 mm debossed label panel at the top left, '
   'three raised rules 5.5 mm apart across the middle and a raised ring 2.8 mm wide round every '
   'hole, no repeat',
   'light 60 degrees round from the left, 50 degrees up; light dots on a jittered 2.6 mm grid '
   'turned 30 degrees hold the flat card at a mid tone, 0.9 to 1.3 mm across; on lit walls the '
   'face goes light with dark pits 1.3 down to 0.7 mm, then solid; shadowed walls and cast '
   'shadows up to 2 mm lose their dots'),
  ('D4', d4_faded,
   'a sun-faded folder: the uncovered upper part faded lighter up to a soft slanting edge, five '
   'scuffed patches about 26 mm across with clusters of scratches 4 to 14 mm long, and a dog-ear '
   'at the top-right corner, a 26 mm flap folded over and lying on the face, no repeat',
   'low sun from the upper left, 22 degrees up; white-line cross-hatching, lines rising to the '
   'right 2.4 mm apart swell from 0.9 to 1.7 mm, then lines falling to the right 2.6 mm apart '
   'join them, then solid; the faded part sits one step lighter and the flap, showing the '
   'unfaded inside, one step darker; scratch walls go solid light on the lower-right side and '
   'dark on the upper-left; the flap has a 0.7 mm light rim on its lit edge and casts a solid '
   'shadow 2 to 3.5 mm long; the cut-away corner is dark'),
]


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
