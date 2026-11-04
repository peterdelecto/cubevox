#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family G: smooth painted metal with metal flake.

Each candidate models a smooth form under one light. The clear coat mirrors a
softbox as a crisp highlight. The flakes are tiny mirrors, each tilted at
random, so they sparkle densest round that highlight and thin out as the form
turns away. The three-dimensional reading comes from where the highlight falls
and how the sparkle density follows it, not from relief.

Each candidate function returns the light-inlay mask on the face grid of
candidates_b.py: 10 px/mm, 1920 x 1520 px, row 0 at the top edge of the face.

  candidates_g.py              every candidate as <name>.png; prints the report lines
  candidates_g.py G0 G3        only the named candidates
  candidates_g.py --draft G0   the raw mask before the printability pass, to /tmp/cane-g
"""

import math
import random
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

from candidates_b import (ALLOWED, H, OUT, S, U, V, W, face_image, hash01, line_dist,
                          printable, wave_noise, zoom)


# --- Light and form -----------------------------------------------------------

def light_dir(u, v, z):
  """Unit vector toward the light: u right, v down the face, z out of it."""
  l = np.array([u, v, z], float)
  return l / np.linalg.norm(l)


def mirror_slope(l):
  """Surface slope (du, dv) at which the clear coat mirrors light from l
  straight up to an eye above the face."""
  return -l[0] / (l[2] + 1), -l[1] / (l[2] + 1)


def slopes(h):
  gv, gu = np.gradient(h, 1 / S)
  return gu, gv


def diffuse(gu, gv, l):
  """Cosine between the surface normal and the light."""
  return np.clip((l[2] - l[0] * gu - l[1] * gv) / np.sqrt(1 + gu * gu + gv * gv), 0, 1)


def softbox(du, dv, a, b, rc, turn):
  """Signed distance in slope space from (du, dv) to a softbox seen in the
  clear coat: a rounded rectangle a by b half-size, corner radius rc, its a
  side turned `turn` degrees from u. Negative inside."""
  c, s = math.cos(math.radians(turn)), math.sin(math.radians(turn))
  x, y = np.abs(c * du + s * dv), np.abs(-s * du + c * dv)
  qx, qy = x - (a - rc), y - (b - rc)
  return (np.hypot(np.maximum(qx, 0), np.maximum(qy, 0))
          + np.minimum(np.maximum(qx, qy), 0) - rc)


def halo(sd, sigma, floor):
  """Share of flakes that catch the light at softbox distance sd: all of them
  at the softbox edge, falling off over sigma (slope units) to `floor`."""
  d = np.maximum(sd, 0)
  return floor + (1 - floor) * np.exp(-d * d / (2 * sigma * sigma))


def sample(a, u, v):
  """Value of face array a at (u, v) mm."""
  return a[min(H - 1, max(0, int(v * S))), min(W - 1, max(0, int(u * S)))]


# --- Flakes -------------------------------------------------------------------

def scatter(seed, radius, gap, rmax, k=24):
  """Flake centres and radii, mm, packed at random over the face so the dark
  gap between neighbours is at least `gap` mm. radius(rng) draws one radius,
  never above rmax."""
  rng = random.Random(seed)
  fw, fh = W / S, H / S
  cell = 2 * rmax + gap
  grid, pts, active = {}, [], []

  def fits(u, v, r):
    if not (0 <= u < fw and 0 <= v < fh):
      return False
    i, j = int(u // cell), int(v // cell)
    return all((pu - u) ** 2 + (pv - v) ** 2 >= (pr + r + gap) ** 2
               for a in (i - 1, i, i + 1) for b in (j - 1, j, j + 1)
               for pu, pv, pr in grid.get((a, b), ()))

  def add(u, v, r):
    pts.append((u, v, r))
    grid.setdefault((int(u // cell), int(v // cell)), []).append((u, v, r))
    active.append(len(pts) - 1)

  add(rng.uniform(0, fw), rng.uniform(0, fh), radius(rng))
  while active:
    n = rng.randrange(len(active))
    pu, pv, pr = pts[active[n]]
    for _ in range(k):
      r = radius(rng)
      d = (pr + r + gap) * (1 + 0.35 * rng.random())
      a = rng.uniform(0, 2 * math.pi)
      u, v = pu + d * math.cos(a), pv + d * math.sin(a)
      if fits(u, v, r):
        add(u, v, r)
        break
    else:
      active[n] = active[-1]
      active.pop()
  return pts


_DISCS = {}


def disc_offsets(rp):
  """Pixel offsets inside a disc of radius rp px centred on a pixel corner."""
  key = round(rp * 4) / 4
  if key not in _DISCS:
    n = int(math.ceil(key))
    dy, dx = np.mgrid[-n:n, -n:n]
    inside = (dy + 0.5) ** 2 + (dx + 0.5) ** 2 <= key * key
    _DISCS[key] = dy[inside], dx[inside]
  return _DISCS[key]


def stamp(mask, u, v, r, value=True):
  """Set a disc of radius r mm at (u, v) mm, centred on the nearest pixel
  corner so a 0.9 mm dot always holds the 0.8 mm test disc."""
  dy, dx = disc_offsets(r * S)
  j, i = dy + round(v * S), dx + round(u * S)
  ok = (j >= 0) & (j < H) & (i >= 0) & (i < W)
  mask[j[ok], i[ok]] = value


def stipple_dots(sites, tone, rng, r0, r1, t0, jitter=0.15):
  """The (u, v, r) dots, mm, that stipple() stamps."""
  dots = []
  for u, v, _ in sites:
    t, x, j = sample(tone, u, v), rng.random(), 1 + jitter * (2 * rng.random() - 1)
    if t >= t0:
      dots.append((u, v, max(r0, (r0 + (r1 - r0) * (t - t0) / (1 - t0)) * j)))
    elif x < t / t0:
      dots.append((u, v, r0))
  return dots


def stipple(sites, tone, rng, r0, r1, t0, jitter=0.15):
  """Light dots on the sites, from the tone (0 to 1) at each site. Below t0 a
  share t / t0 of the sites lights, each a smallest dot of radius r0 mm. From
  t0 up every site lights and its dot grows toward r1 mm, so neighbours merge
  and the dark between them shrinks to specks."""
  light = np.zeros(U.shape, bool)
  for u, v, r in stipple_dots(sites, tone, rng, r0, r1, t0, jitter):
    stamp(light, u, v, r)
  return light


def specks(light, sites, sd, rng, r, edge, deep):
  """Dark specks inside the clear-coat highlight (sd < 0): flakes turned away
  from the light. A share `edge` of the sites at the highlight's rim, `deep`
  at its middle."""
  for u, v, _ in sites:
    d, x = sample(sd, u, v), rng.random()
    if d < 0 and x < deep + (edge - deep) * math.exp(-d * d / (2 * 0.025 ** 2)):
      stamp(light, u, v, r, False)


def window(u, v, reach):
  """Pixel slices and mm coordinates, relative to (u, v), of a square reaching
  `reach` mm round (u, v), clipped to the face."""
  ci, cj, n = round(u * S), round(v * S), int(reach * S) + 2
  i0, i1, j0, j1 = max(0, ci - n), min(W, ci + n), max(0, cj - n), min(H, cj + n)
  x = (np.arange(i0, i1) + 0.5) / S - ci / S
  y = (np.arange(j0, j1) + 0.5) / S - cj / S
  return (slice(j0, j1), slice(i0, i1)), x[None, :], y[:, None]


def glint(mask, u, v, arm, core, ring):
  """A four-point glint at (u, v) mm: arms `arm` mm long along u and v,
  narrowing from `core` mm at the centre to a 1.0 mm rounded tip on concave
  flanks, cut free of its surroundings by a dark ring `ring` mm wide."""
  sl, x, y = window(u, v, arm + ring + 1)
  dist = np.full(np.broadcast_shapes(x.shape, y.shape), np.inf)
  for a, b in ((x, y), (y, x)):
    along = np.minimum(np.abs(a), arm)
    half = (1.0 + (core - 1.0) * (1 - along / arm) ** 2) / 2
    dist = np.minimum(dist, np.hypot(np.abs(a) - along, b) - half)
  sub = mask[sl]
  sub[dist < ring] = False
  sub[dist < 0] = True


def polygon(draw, u, v, r, sides, turn, rng, fill):
  """An irregular flat flake: `sides` corners round (u, v) mm at 0.85 to 1.1 r."""
  pts = []
  for k in range(sides):
    a = turn + 2 * math.pi * (k + 0.3 * (rng.random() - 0.5)) / sides
    rr = r * (0.85 + 0.25 * rng.random())
    pts.append((u * S - 0.5 + rr * S * math.cos(a), v * S - 0.5 + rr * S * math.sin(a)))
  draw.polygon(pts, fill=fill)


# --- G0: fine flake over a low dome --------------------------------------------

def g0_fine():
  """Fine metal flake over a low dome, lit from the upper left. The clear
  coat mirrors a tilted softbox as a solid highlight on the dome's upper-left
  shoulder, specked dark by flakes turned away. Flakes are 0.9 mm dots, the
  printable limit, at least 1.65 mm apart. Round the highlight every flake
  lights and the dots swell until they merge; farther off fewer flakes catch,
  thinning to a scatter on the dome's far side."""
  l = light_dir(-1, -1, 1.3)
  su, sv = mirror_slope(l)
  x, y = (U - 104) / 96, (V - 84) / 76
  h = -(24 * x * x + 20 * y * y) + 0.3 * wave_noise(U, V, 61, 90)
  gu, gv = slopes(h)
  sd = softbox(gu - su, gv - sv, 0.12, 0.07, 0.05, -30)
  sites = scatter(60, lambda _: 0.45, 0.75, 0.45)
  rng = random.Random(62)
  light = stipple(sites, 0.55 * halo(sd, 0.22, 0.4), rng, 0.45, 1.3, 0.35) | (sd < 0)
  specks(light, sites, sd, rng, 0.36, 0.8, 0.12)
  return light


def g5_dots():
  """G5's (u, v, r) dots, mm, before the face masks."""
  l = light_dir(-1, -1, 1.3)
  su, sv = mirror_slope(l)
  x, y = (U - 104) / 96, (V - 84) / 76
  h = -(24 * x * x + 20 * y * y) + 0.3 * wave_noise(U, V, 61, 90)
  gu, gv = slopes(h)
  sd = softbox(gu - su, gv - sv, 0.12, 0.07, 0.05, -30)
  sites = scatter(60, lambda _: 0.45, 0.75, 0.45)
  rng = random.Random(62)
  return stipple_dots(sites, 0.55 * halo(sd, 0.22, 0.4), rng, 0.45, 0.50, 0.35, jitter=0.04)


def g5_fine_open():
  """Fine metal flake over a low dome, lit from the upper left, with no solid
  highlight. The flakes sit where G0's do, and the clear coat's softbox
  reflection sets each one's size and density. Flakes stay at most 1.04 mm
  across, so none merge, even where the highlight was."""
  light = np.zeros(U.shape, bool)
  for u, v, r in g5_dots():
    stamp(light, u, v, r)
  return light


def g5_circle_list(edge=0.15):
  """(u, v, r) circles in mm on the face grid for G5: each dot centred where
  stamp() centres it, kept only when the dot plus `edge` mm stays in ALLOWED."""
  from candidates_c import leaves_allowed
  out = []
  for u, v, r in g5_dots():
    u, v = round(u * S) / S, round(v * S) / S
    if not leaves_allowed(u, v, r + edge):
      out.append((u, v, r))
  return out


# --- G1: coarse flake with a few big glints -------------------------------------

def g1_coarse():
  """Coarse flake over a horizontal swell, lit from straight above. A long
  strip light mirrors in the clear coat as a solid band across the upper
  flank of the swell, and the floor below lights the underside faintly.
  Flat irregular flakes 1.4 to 2.4 mm across catch in a broad halo round the
  band, growing as they near it, and the flakes nearest the band flare into
  four-point glints."""
  l = light_dir(0, -1, 1.1)
  su, sv = mirror_slope(l)
  crease = 84 + 7 * np.sin(U / 192 * 1.3 * math.pi + 0.4)
  s = (V - crease) / 34
  h = -30 * np.log(np.cosh(s)) - 0.0009 * (U - 96) ** 2 + 0.4 * wave_noise(U, V, 71, 90)
  gu, gv = slopes(h)
  sd = softbox(gu - su, gv - sv, 0.3, 0.06, 0.04, 0)
  floor = softbox(gu, gv + 0.62, 0.4, 0.06, 0.04, 0)
  tone = np.maximum(halo(sd, 0.2, 0.16), 0.5 * halo(floor, 0.12, 0))
  mask = Image.fromarray((sd < 0).astype(np.uint8) * 255)
  draw = ImageDraw.Draw(mask)
  rng = random.Random(72)
  near = []
  for u, v, r in scatter(70, lambda g: g.uniform(0.7, 1.2), 0.75, 1.2):
    d, t, x = sample(sd, u, v), sample(tone, u, v), rng.random()
    sides, turn = rng.randint(5, 7), rng.uniform(0, 2 * math.pi)
    if d < 0:
      if x < 0.15 + 0.6 * math.exp(-d * d / (2 * 0.025 ** 2)):
        polygon(draw, u, v, r * 0.62, sides, turn, rng, 0)
    elif x < t / 0.6:
      polygon(draw, u, v, r * (1 + 0.5 * max(0.0, t - 0.6) / 0.4), sides, turn, rng, 255)
      near.append((d + 0.12 * rng.random(), u, v))
  light = np.asarray(mask) > 127

  # The glints: catching flakes nearest the band, kept 14 mm apart, the
  # largest nearest the band.

  placed = []
  for d, u, v in sorted(near):
    if len(placed) == 26:
      break
    if all((u - pu) ** 2 + (v - pv) ** 2 > 14 ** 2 for pu, pv in placed):
      size = 1 - min(1.0, d / 0.3)
      glint(light, u, v, 3.0 + 3.5 * size * rng.uniform(0.7, 1.0), 2.2, 0.75)
      placed.append((u, v))
  return light


# --- G2: a curved panel with a sweeping highlight -------------------------------

def g2_sweep():
  """Flake paint over a long curved panel, a body line rising from lower left
  to upper right, lit from the upper left. Across the line the panel's height
  is h(s) = m s - 48 log cosh(s / 80) mm, so its slope falls from steep on the
  upper shoulder to nearly flat on the lower flank, and the clear coat mirrors
  a softbox as a solid highlight sweeping the length of the line. Engraved
  lines 2 mm apart run along the panel; they swell toward the highlight and
  thin on the turned-away flank. Flakes catching the light bead the lines,
  densest in the sweep; turned-away flakes speck the solid highlight."""
  l = light_dir(-0.6, -1, 0.8)
  su, sv = mirror_slope(l)
  m = math.hypot(su, sv)
  nu, nv = su / m, sv / m
  along = (U - 96) * nv - (V - 76) * nu
  s = (U - 96) * nu + (V - 76) * nv + 0.0018 * along ** 2 + 2.0 * wave_noise(U, V, 81, 120)
  ds_v, ds_u = np.gradient(s, 1 / S)
  fall = m - 0.6 * np.tanh(s / 80)
  gu, gv = fall * ds_u, fall * ds_v
  sd = softbox(gu - su, gv - sv, 0.05, 0.2, 0.04, math.degrees(math.atan2(nv, nu)))
  lit = np.clip((diffuse(gu, gv, l) - 0.5) / 0.45, 0, 1)
  tone = np.maximum(0.9 * halo(sd, 0.15, 0), 0.38 * lit)

  # Lines follow s; their true spacing comes from the gradient of s. Below
  # tone `full` a line breaks into 5 mm dashes, a share tone / full of them kept.

  stretch = np.hypot(ds_u, ds_v)
  pitch, full = 2.2, 0.35
  dist = line_dist(s, pitch) / stretch
  k = np.clip((tone - full) / (0.9 - full), 0, 1)
  width = 0.9 + k * (pitch - 1.6)
  line = np.round(s / pitch).astype(np.int64)
  dash = hash01(line, np.floor((along + 40 * hash01(line, 1)) / 8).astype(np.int64))
  light = ((dist < width / 2) & (dash < tone / full)) | (sd < 0)
  width = np.where(dash < tone / full, width, 0.0)

  sites = scatter(80, lambda _: 0.5, 1.0, 0.5)
  rng = random.Random(82)
  specks(light, sites, sd, rng, 0.36, 0.8, 0.1)
  for u, v, _ in sites:
    d, x = sample(sd, u, v), rng.random()
    if d < 0 or x >= halo(d, 0.14, 0.04):
      continue
    w = sample(width, u, v)
    if w == 0:
      stamp(light, u, v, 0.45)
      continue

    # A bead sits on the nearest line, as wide as the gap to the next line allows.

    st, sv_ = sample(stretch, u, v), sample(s, u, v)
    off = (sv_ - pitch * round(sv_ / pitch)) / st
    bu, bv = sample(ds_u, u, v) / st, sample(ds_v, u, v) / st
    bead = min(1.5, 2 * (pitch / st - 0.75) - w)
    if bead > w + 0.25:
      stamp(light, u - off * bu, v - off * bv, bead / 2)
  return light


# --- G3: hammered-finish paint --------------------------------------------------

def g3_hammered():
  """Hammered-finish paint: shallow dimples 4.5 to 7.5 mm across over a broad
  dome, lit from the upper left. Inside each dimple the lower-right wall
  faces the light and the upper-left wall falls into shade. The dome sets
  how much of each dimple is light: on its lit shoulder the ground is light,
  broken by dark crescents, and on its far side dark, broken by lit ones.
  Each dimple also mirrors the softbox as a small glint, which only lands
  inside a dimple where the dome faces the light."""
  l = light_dir(-1, -1, 1.1)
  su, sv = mirror_slope(l)
  rng = random.Random(92)

  # Each dimple is a bowl -D (1 - (d / R)^2)^2, wider than its share of the
  # face so neighbours cut into each other along curved ridges.

  dimple = np.zeros(U.shape)
  for u, v, r in scatter(90, lambda g: g.uniform(2.2, 3.7), 0.0, 3.7):
    big = 1.45 * r * rng.uniform(0.9, 1.1)
    sl, x, y = window(u, v, big)
    q = np.clip(1 - (x * x + y * y) / (big * big), 0, 1)
    dimple[sl] = np.minimum(dimple[sl], -0.33 * big * rng.uniform(0.8, 1.2) * q * q)
  x, y = (U - 100) / 96, (V - 80) / 76
  dome = -(26 * x * x + 22 * y * y)
  dgu, dgv = slopes(dimple)
  bgu, bgv = slopes(dome)

  # Each pixel is light if its dimple-wall lighting ranks in the top share
  # the dome sets there: 86 % on the lit shoulder down to 14 % on the far side.

  lit = diffuse(dgu, dgv, l)
  cover = 0.14 + 0.72 * halo(softbox(bgu - su, bgv - sv, 0.05, 0.05, 0.04, 0), 0.3, 0)
  q = np.sort(lit[ALLOWED])
  light = lit > q[((1 - cover) * (q.size - 1)).astype(int)]
  return light | (softbox(dgu + bgu - su, dgv + bgv - sv, 0.09, 0.09, 0.07, 0) < 0)


# --- G4: flake over brushed metal ----------------------------------------------

def g4_brushed():
  """Translucent flake paint over brushed metal, on a low dome lit from the
  upper right. The brushing runs across the face, so its sheen stretches
  into a tall streak at right angles to it, over a fainter round sheen from
  the dome. Brush scratches run the width of the face, light lines on a 2.0
  to 2.3 mm pitch. Each scratch shows where the sheen beats its own random
  threshold, so scratches lengthen and join toward the streak and their
  ends trace its edge; each swells from 0.9 to 1.15 mm wide as the sheen
  rises. In the streak the dark between the lines breaks up the same way,
  leaving solid light crossed by dark scratches. The flakes sparkle in a
  rounder halo, each a 1.3 mm dot strung on a scratch line."""
  l = light_dir(1, -0.8, 1.4)
  su, sv = mirror_slope(l)
  x, y = (U - 50) / 96, (V - 82) / 76
  h = -(22 * x * x + 18 * y * y) + 0.5 * wave_noise(U, V, 101, 90)
  gu, gv = slopes(h)
  du, dv = gu - su, gv - sv
  streak = np.exp(-du * du / (2 * 0.06 ** 2) - dv * dv / (2 * 0.3 ** 2))
  sheen = np.clip(0.12 + 0.3 * np.exp(-(du * du + dv * dv) / (2 * 0.25 ** 2)) + 0.6 * streak, 0, 1)

  light = np.zeros(U.shape, bool)
  rng = random.Random(102)
  fw, fh = W / S, H / S
  cols = (np.arange(W) + 0.5) / S

  def scratches(level):
    """Columns lit along one scratch line: scratches 2 to about 40 mm long,
    a third parted from the next by a gap, each lit where `level` (per
    column) beats its own random threshold."""
    ends, gaps, u = [], [], -rng.uniform(0, 20)
    while u < fw:
      u += 2 + rng.expovariate(1 / 16)
      ends.append(u)
      gaps.append(0.8 + 0.8 * rng.random() if rng.random() < 0.35 else 0.0)
    ends, gaps = np.array(ends), np.array(gaps)
    seg = np.searchsorted(ends, cols)
    return (np.array([rng.random() for _ in ends])[seg] < level) & (cols < ends[seg] - gaps[seg])

  rows, v = [], 0.6
  while v < fh:
    rows.append(v + 1.0)
    v += rng.uniform(2.0, 2.3)
  rows = np.array(rows)
  for k, v in enumerate(rows):
    t = sheen[min(H - 1, int(v * S))]
    on = scratches(np.minimum(1, t / 0.6))
    w = 0.9 + 0.25 * np.minimum(1, t / 0.6)
    for j in range(max(0, int((v - 0.7) * S)), min(H, int((v + 0.7) * S) + 1)):
      light[j] |= on & (abs((j + 0.5) / S - v) < w / 2)
    if k + 1 < len(rows):
      t = sheen[min(H - 1, int((v + rows[k + 1]) / 2 * S))]
      fill = scratches(0.9 * np.clip((t - 0.6) / 0.4, 0, 1))
      light[int(v * S):min(H, int(rows[k + 1] * S))] |= fill
  sd = softbox(du, dv, 0.04, 0.04, 0.03, 0)
  for fu, fv, _ in scatter(100, lambda _: 0.65, 2.2, 0.65):
    if rng.random() < halo(sample(sd, fu, fv), 0.17, 0.07):
      stamp(light, fu, rows[np.argmin(np.abs(rows - fv))], 0.65)
  return light


# --- Output -------------------------------------------------------------------

CANDIDATES = [
  ('G0', g0_fine, '', ''),
  ('G1', g1_coarse, '', ''),
  ('G2', g2_sweep, '', ''),
  ('G3', g3_hammered, '', ''),
  ('G4', g4_brushed, '', ''),
  ('G5', g5_fine_open, 'fine flake dots at G0 density, sized and thinned by the softbox reflection with no solid highlight', 'a softbox reflected in the clear coat over a low dome lit from the upper left, shown only by dot size and density'),
]


def render(name, fn):
  t0 = time.time()
  light, changed = printable(fn())
  img = face_image(light)
  img.save(OUT / f'{name}.png')
  print(f'{name}: {changed * 100:.2f} % of light pixels changed, {time.time() - t0:.1f} s')
  return img, changed


def draft(name, fn):
  """The raw mask, before the printability pass, at full size and as a zoom."""
  d = Path('/tmp/cane-g')
  d.mkdir(exist_ok=True)
  light = fn() & ALLOWED
  img = face_image(light)
  img.save(d / f'{name}.png')
  zoom(img).save(d / f'{name}-zoom.png')
  cover = np.count_nonzero(light) / np.count_nonzero(ALLOWED)
  print(f'{name}: draft in {d}, {cover * 100:.1f} % light')


def main(args):
  if args and args[0] == '--draft':
    for name, fn, _, _ in CANDIDATES:
      if name in args[1:] or len(args) == 1:
        draft(name, fn)
    return
  OUT.mkdir(parents=True, exist_ok=True)
  chosen = [c for c in CANDIDATES if not args or c[0] in args]
  results, lines = [], []
  for name, fn, what, how in chosen:
    img, changed = render(name, fn)
    results.append((name, img))
    lines.append(f'{name}: {what}. Highlight and shadow: {how}. '
                 f'Printability pass changed {changed * 100:.2f} % of light pixels.')
  print('\n'.join(lines))


if __name__ == '__main__':
  main(sys.argv[1:])
