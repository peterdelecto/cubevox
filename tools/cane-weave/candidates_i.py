#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family I: separate light circles only.

Every light mark is one circle on dark ground (C12's rule). Each circle has a
radius the print opening leaves whole, 0.47 to 1.33 mm, keeps a 0.65 mm dark
gap to every neighbour and sits clear of the holes and the OLED. The face
grid, the hole geometry, the printability pass and the output image come from
candidates_b.py; C12's radius fit and tissue lighting come from candidates_c.py.

  candidates_i.py           every candidate's face PNG; prints the report lines
  candidates_i.py I0 I3     only the named candidates
"""

import math
import random
import subprocess
import sys
from functools import lru_cache

import numpy as np
from scipy.ndimage import maximum_filter, zoom
from scipy.spatial import cKDTree

from candidates_b import ALLOWED, H, LIGHT, OUT, S, U, V, W, blur, render
from candidates_c import Sheet, fit, leaves_allowed, light_dir, lit, tone
from candidates_g import scatter, stamp

GAP, EDGE = 0.65, 0.15        # dark gap between circles; margin inside the field, mm
R_MIN, R_MAX = 0.47, 1.33     # circle radius limits, mm


# --- Shared tools -----------------------------------------------------------

def tone_at(t):
  return lambda u, v: t[min(H - 1, int(v * S)), min(W - 1, int(u * S))]


def inside(u, v, r):
  return not leaves_allowed(u, v, r + EDGE)


def draw(circles):
  light = np.zeros((H, W), bool)
  for u, v, r in circles:
    stamp(light, u, v, r)
  return light


class Room:
  """Distance in mm from every pixel corner to the nearest placed circle's
  edge, kept exact up to the reach of the largest circle plus the gap.
  Circle centres sit on pixel corners, so a lookup is exact."""

  def __init__(self, circles=()):
    self.clear = np.full((H + 1, W + 1), 9.0)
    for c in circles:
      self.add(*c)

  def room(self, u, v):
    """Largest radius a circle at (u, v) may take, capped at R_MAX."""
    return min(R_MAX, self.clear[round(v * S), round(u * S)] - GAP)

  def add(self, u, v, r):
    j0, i0 = round(v * S), round(u * S)
    n = int(math.ceil((r + R_MAX + GAP) * S)) + 1
    ja, jb, ia, ib = max(0, j0 - n), min(H + 1, j0 + n + 1), max(0, i0 - n), min(W + 1, i0 + n + 1)
    jj, ii = np.ogrid[ja:jb, ia:ib]
    d = np.hypot(jj / S - v, ii / S - u) - r
    np.minimum(self.clear[ja:jb, ia:ib], d, out=self.clear[ja:jb, ia:ib])


def dots_from_tone(t, seed):
  """C12's placement on any tone field t in [0, 1]. Big circles sit on a
  Poisson site set 2.4 to 2.7 mm apart and grow with the tone from 0.9 mm to
  the room their neighbours leave less 0.65 mm, thinning to bare dark below
  tone 0.2. Above tone 0.45, circles of 0.94 to 1.3 mm fill the gaps wherever
  a 0.7 mm dark gap to every neighbour remains. Returns (u, v, r) in mm."""
  ts = tone_at(t)
  rng = random.Random(seed + 1)
  raw = scatter(seed, lambda r: r.uniform(0.6, 0.75), 1.2, 0.75, k=80)
  sites = np.array([(round(u * S) / S, round(v * S) / S) for u, v, _ in raw])
  nn = cKDTree(sites).query(sites, k=2)[0][:, 1]

  circles = []
  for (u, v), n in zip(sites, nn):
    x, tt = rng.random(), ts(u, v)
    if tt < 0.2:
      d = 0.9 if x < tt / 0.2 else 0
    else:
      d = 0.9 + (max(0.9, n - GAP) - 0.9) * min(1, (tt - 0.2) / 0.4)
    r = fit(min(d / 2, R_MAX))
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
    room = 0.45 + 0.2 * min(1, (tt - 0.45) / 0.4)
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


def light_tone(h):
  """C12's lighting: tone of the height field h lit from the upper right."""
  gv, gu = np.gradient(h, 1 / S)
  return blur(tone(lit(gu, gv, light_dir(315, 35))), 0.3)


@lru_cache(None)
def tissue():
  """C4's crumpled tissue: its height h and C12's lit tone t."""
  h = blur(Sheet(12.0, 4.5, 141).h, 1.5)
  return h, light_tone(h)


# --- I0: frost --------------------------------------------------------------

@lru_cache(None)
def i0_list():
  """Diffusion-limited aggregation on a hex lattice, cells 2.0 mm apart in
  rows 1.8 mm apart, so every cell centre falls on a pixel corner."""
  rng = random.Random(201)
  rmax = fit(0.67)
  cols, rows = int(W / S / 2.0) + 1, int(H / S / 1.8) + 1
  pos = [(round((2.0 * i + 1.0 * (j % 2)) * S) / S, round(1.8 * j * S) / S)
         for j in range(rows) for i in range(cols)]
  free = [inside(u, v, rmax) for u, v in pos]

  def nbrs(j, i):
    s = j % 2
    out = [(j, i - 1), (j, i + 1), (j - 1, i - 1 + s), (j - 1, i + s),
           (j + 1, i - 1 + s), (j + 1, i + s)]
    return [b * cols + a for b, a in out if 0 <= b < rows and 0 <= a < cols]

  nb = [nbrs(c // cols, c % cols) for c in range(len(pos))]

  # The seeds are the blocked cells (field border, hole and OLED clearance);
  # a free cell touching a seed or a stuck cell catches the next walker.

  touch = [free[c] and (len(nb[c]) < 6 or not all(free[n] for n in nb[c]))
           for c in range(len(pos))]
  stuck = [False] * len(pos)

  # Lattice steps from each free cell to the nearest seed or stuck cell.
  # Walkers start as far out as the growth allows, so the tips screen the
  # fjords and the branches stay long and thin.

  dist = [0 if not free[c] else 1 if touch[c] else 99 for c in range(len(pos))]

  def spread(front):
    while front:
      nxt = []
      for c in front:
        for n in nb[c]:
          if free[n] and dist[c] + 1 < dist[n]:
            dist[n] = dist[c] + 1
            nxt.append(n)
      front = nxt

  spread([c for c in range(len(pos)) if dist[c] == 1])
  open_cells = [c for c in range(len(pos)) if dist[c] >= 2]
  order = []
  N, reach, misses = 3000, 8, 0
  while len(order) < N and open_cells and reach >= 3:
    n = rng.randrange(len(open_cells))
    c = open_cells[n]
    if dist[c] < 2:
      open_cells[n] = open_cells[-1]
      open_cells.pop()
      continue
    if dist[c] < reach:
      misses += 1
      if misses > 4000:
        reach, misses = reach - 1, 0
      continue
    misses = 0
    while not touch[c]:
      c = nb[c][int(rng.random() * 6)]
    stuck[c], touch[c], dist[c] = True, False, 0
    order.append(c)
    for n in nb[c]:
      if free[n] and not stuck[n]:
        touch[n] = True
    spread([c])

  circles = []
  for age, c in enumerate(order):
    u, v = pos[c]
    r = fit(0.47 + 0.20 * (1 - age / len(order)))
    if r >= R_MIN and inside(u, v, r):
      circles.append((u, v, r))
  return circles


def i0_frost():
  return draw(i0_list())


# --- I1: coral stripes ------------------------------------------------------

def gray_scott(steps, F, k, seed, ny, nx, du=0.16, dv=0.08, seeds=12):
  """V of a Gray-Scott reaction on an ny x nx grid; stripes repeat about
  every 12 cells."""
  rng = np.random.default_rng(seed)
  a = np.ones((ny, nx), np.float32)
  b = np.zeros((ny, nx), np.float32)
  for _ in range(seeds):
    j, i = rng.integers(0, ny - 10), rng.integers(0, nx - 10)
    a[j:j + 10, i:i + 10] = 0.5
    b[j:j + 10, i:i + 10] = 0.25
  a += 0.02 * rng.random((ny, nx), np.float32)
  b += 0.02 * rng.random((ny, nx), np.float32)

  def lap(x):
    return (np.roll(x, 1, 0) + np.roll(x, -1, 0) + np.roll(x, 1, 1) + np.roll(x, -1, 1) - 4 * x)

  for _ in range(steps):
    abb = a * b * b
    a += du * lap(a) - abb + F * (1 - a)
    b += dv * lap(b) + abb - (F + k) * b
  return b


@lru_cache(None)
def i1_list():
  """Gray-Scott at 0.75 mm per cell over the field, so the stripes run about
  4.6 mm wide with dark lanes as wide between them."""
  b = zoom(gray_scott(12000, 0.055, 0.062, 211, 195, 248, seeds=24), 7.5, order=1)
  b = (b - b.min()) / (b.max() - b.min())
  t = np.zeros((H, W))
  t[30:30 + b.shape[0], 30:30 + b.shape[1]] = b
  return dots_from_tone(t, 212)


def i1_coral():
  return draw(i1_list())


# --- I2: sunflower spirals --------------------------------------------------

@lru_cache(None)
def i2_list():
  """Phyllotaxis whorls round the tissue's broad crowns, grown one seed per
  whorl per turn so neighbouring whorls meet halfway."""
  h, t = tissue()
  ts = tone_at(t)
  hb = blur(h, 4.0)
  in_field = (U >= 3.0) & (U <= 189.0) & (V >= 3.0) & (V <= 149.0)
  peak = (maximum_filter(hb, size=5 * S) == hb) & in_field
  js, is_ = np.nonzero(peak)
  centres = []
  for k in np.argsort(-hb[js, is_]):
    u, v = (is_[k] + 0.5) / S, (js[k] + 0.5) / S
    if all(math.hypot(u - cu, v - cv) >= 25.0 for cu, cv in centres):
      centres.append((u, v))

  # The tissue has too few crowns for 25 whorls; the rest go where the
  # field lies farthest from every centre.

  while len(centres) < 25:
    far = np.full((H, W), np.inf)
    for cu, cv in centres:
      np.minimum(far, np.hypot(U - cu, V - cv), out=far)
    far[~ALLOWED] = 0
    j, i = np.unravel_index(far.argmax(), far.shape)
    if far[j, i] < 25.0:
      break
    centres.append(((i + 0.5) / S, (j + 0.5) / S))

  room = Room()
  circles = []
  misses = [0] * len(centres)
  active = list(range(len(centres)))
  k = 0
  while active:
    k += 1
    rad, ang = 1.45 * math.sqrt(k), math.radians(k * 137.508)
    for c in list(active):
      cu, cv = centres[c]
      u, v = round((cu + rad * math.cos(ang)) * S) / S, round((cv + rad * math.sin(ang)) * S) / S
      placed = False
      if 0 <= u <= W / S and 0 <= v <= H / S:
        r = fit(0.47 + 0.86 * ts(u, v))
        if r >= R_MIN and room.room(u, v) >= r - 1e-9 and inside(u, v, r):
          room.add(u, v, r)
          circles.append((u, v, r))
          placed = True
      misses[c] = 0 if placed else misses[c] + 1
      if misses[c] > 80 or rad > 80:
        active.remove(c)
  i2_list.centres = len(centres)
  return circles


def i2_sunflower():
  return draw(i2_list())


# --- I3: foam packing -------------------------------------------------------

@lru_cache(None)
def i3_list():
  _, t = tissue()
  ts = tone_at(t)
  circles = dots_from_tone(t, 301)
  room = Room(circles)
  rng = random.Random(302)
  cand = [(i * 3 / S, j * 3 / S) for i in range(W // 3 + 1) for j in range(H // 3 + 1)]
  rng.shuffle(cand)
  for u, v in cand:
    if rng.random() >= min(1.0, max(0.0, (ts(u, v) - 0.15) / 0.5)):
      continue
    r = fit(room.room(u, v))
    while r >= R_MIN and not inside(u, v, r):
      r = fit(r - 0.01)
    if r >= R_MIN:
      room.add(u, v, r)
      circles.append((u, v, r))
  return circles


def i3_foam():
  return draw(i3_list())


# --- I4: body folds ---------------------------------------------------------

def smin(a, b, k):
  """Polynomial soft minimum, rounding the corner over |a - b| < k."""
  q = np.maximum(k - np.abs(a - b), 0) / k
  return np.minimum(a, b) - q * q * k / 4


@lru_cache(None)
def i4_list():
  """Eight long smooth ridges on a jittered 4 x 2 grid. Each pair is joined
  by its soft maximum (sum less soft minimum), so a soft crease runs where
  two ridges meet."""
  rng = np.random.default_rng(401)
  ridges = []
  for gj in range(2):
    for gi in range(4):
      cu = 3.0 + (gi + rng.uniform(0.25, 0.75)) * 186 / 4
      cv = 3.0 + (gj + rng.uniform(0.25, 0.75)) * 146 / 2
      length, width = rng.uniform(40, 90), rng.uniform(15, 30)
      ang, amp = rng.uniform(0, math.pi), rng.uniform(2, 4)
      s = (U - cu) * math.cos(ang) + (V - cv) * math.sin(ang)
      q = -(U - cu) * math.sin(ang) + (V - cv) * math.cos(ang)
      ridges.append(amp * np.exp(-0.5 * ((s / (length / 2)) ** 2 + (q / (width / 2)) ** 2)))
  h = sum(ridges)
  for a in range(len(ridges)):
    for b in range(a + 1, len(ridges)):
      h -= smin(ridges[a], ridges[b], 0.5)
  h = blur(h, 1.5)
  return dots_from_tone(light_tone(h), 402)


def i4_folds():
  return draw(i4_list())


# --- Report -----------------------------------------------------------------

CANDIDATES = [
  ('I0', i0_frost,
   'frost or lichen grown by diffusion-limited aggregation from the field border and every hole '
   'ring, one circle per stuck cell of a 2 mm hex lattice',
   'no light: trunks that stuck first are 1.34 mm across, tips that stuck last are pinpricks'),
  ('I1', i1_coral,
   'Gray-Scott coral stripes about 3 to 5 mm wide drawn as beaded worms of circles',
   'the stripe strength sets the tone: circles swell in the stripe cores and vanish between'),
  ('I2', i2_sunflower,
   'sunflower spirals at 137.5 degrees round the broad crowns of the C4 tissue, whorls tiling '
   'the field',
   'light from the upper right on the tissue sizes each seed circle from 0.94 to 2.66 mm'),
  ('I3', i3_foam,
   'C12 tissue circles packed on into foam, big circles ringed by ever-smaller ones',
   'light from the upper right packs the crowns densest; the shadows stay bare dark'),
  ('I4', i4_folds,
   'long smooth body folds with soft creases where they meet, a torso in raking light',
   'light from the upper right swells circles over the curves and thins them into the creases'),
]

CIRCLES = {'I0': i0_list, 'I1': i1_list, 'I2': i2_list, 'I3': i3_list, 'I4': i4_list}


def gates(circles):
  """Smallest dark gap between any two circles, mm, and the gate c misses."""
  c = np.array(circles)
  pairs = cKDTree(c[:, :2]).query_pairs(2 * R_MAX + GAP + 0.01, output_type='ndarray')
  d = np.hypot(*(c[pairs[:, 0], :2] - c[pairs[:, 1], :2]).T)
  gap = (d - c[pairs[:, 0], 2] - c[pairs[:, 1], 2]).min()
  return gap, sum(not inside(u, v, r) for u, v, r in circles)


def main(names):
  OUT.mkdir(parents=True, exist_ok=True)
  field = np.count_nonzero(ALLOWED)
  chosen = [c for c in CANDIDATES if not names or c[0] in names]
  lines = []
  for name, fn, what, how in chosen:
    img, changed = render(name, fn)
    circles = CIRCLES[name]()
    lit_px = np.all(np.asarray(img) == LIGHT, axis=2)
    gap, outside = gates(circles)
    radii = [r for _, _, r in circles]
    md5 = subprocess.run(['md5', '-q', str(OUT / f'{name}.png')], capture_output=True,
                         text=True, check=True).stdout.strip()
    line = (f'{name}: {len(circles)} dots, coverage {100 * lit_px.sum() / field:.1f} %, '
            f'min gap {gap:.3f} mm, changed {changed * 100:.2f} %, md5 {md5}')
    misses = []
    if gap < GAP - 1e-6:
      misses.append(f'gate b: min gap {gap:.3f} mm')
    if min(radii) < R_MIN or max(radii) > R_MAX:
      misses.append(f'gate a: radii {min(radii):.2f} to {max(radii):.2f} mm')
    if outside:
      misses.append(f'gate c: {outside} circles leave the field')
    if changed > 0.005:
      misses.append(f'gate d: changed {changed * 100:.2f} %')
    if not 1500 <= len(circles) <= 6000:
      misses.append(f'gate e: {len(circles)} dots')
    lines.append(line + ''.join(f'\n  {name} {m}' for m in misses))
    print(lines[-1], flush=True)
    print(f'  {name}: {what}. Highlight and shadow: {how}.', flush=True)


if __name__ == '__main__':
  main(sys.argv[1:])
