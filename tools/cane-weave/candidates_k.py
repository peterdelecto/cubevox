#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family K: uniform squares at any angle.

Every light mark is a square of side SIDE at its own rotation. Tone shows only
as density and order: lit regions lock into tight aligned packing, shadows
thin and scatter, and the shadows proper stay bare dark. Any two squares keep
a 0.7 mm gap measured on their four edge normals. The patterns come from
I0 to I4 (candidates_i.py) and C12's tissue; the face grid, the image and the
output folder from candidates_b.py.

  candidates_k.py           every candidate's face PNG; prints the report lines
  candidates_k.py K0 K3     only the named candidates
"""

import math
import random
import subprocess
import sys
import time

import numpy as np
from PIL import Image, ImageDraw
from scipy.ndimage import binary_closing, distance_transform_edt, maximum_filter, zoom
from scipy.spatial import cKDTree

from candidates_b import ALLOWED, CLOSE_DISC, H, OUT, S, U, V, W, blur, closing, face_image
from candidates_i import gray_scott, i0_list, light_tone, smin, tissue

SIDE = 1.4                          # square side, mm
HALF = SIDE / 2
GAP = 0.7                           # dark gap between squares, mm
REACH = SIDE * math.sqrt(2) + GAP   # centres farther apart than this never clash
CELL = REACH                        # neighbour grid, mm
BLOCK = 8.0                         # lattice block, mm; one angle per block

# Lattice pitch, mm: room for a neighbour jittered 6.75 degrees (tone 0.85).
# One pitch for every block keeps blocks of equal angle on one lattice.

PITCH = GAP + SIDE * (math.cos(math.radians(6.75)) + math.sin(math.radians(6.75)))
ALLOWED_IN = distance_transform_edt(ALLOWED) > 0.15 * S


# --- Squares ----------------------------------------------------------------

def corners(u, v, th):
  c, s = HALF * math.cos(th), HALF * math.sin(th)
  return [(u + c - s, v + s + c), (u - c - s, v - s + c), (u - c + s, v - s - c), (u + c + s, v + s - c)]


def gap(a, b):
  """Largest interval gap, mm, of two squares (u, v, th) projected on their
  four edge normals."""
  du, dv = b[0] - a[0], b[1] - a[1]
  best = -9.0
  for phi in (a[2], a[2] + math.pi / 2, b[2], b[2] + math.pi / 2):
    ea = HALF * (abs(math.cos(a[2] - phi)) + abs(math.sin(a[2] - phi)))
    eb = HALF * (abs(math.cos(b[2] - phi)) + abs(math.sin(b[2] - phi)))
    best = max(best, abs(du * math.cos(phi) + dv * math.sin(phi)) - ea - eb)
  return best


def in_field(u, v):
  j, i = int(v * S), int(u * S)
  return 0 <= j < H and 0 <= i < W and ALLOWED_IN[j, i]


class Placer:
  """Greedy placement under the gap rule and the shrunk field."""

  def __init__(self):
    self.grid, self.squares = {}, []

  def try_add(self, u, v, th):
    # Corners, plus quarter points along each edge so the OLED clearance's
    # corner cannot poke into an edge between two square corners.

    cs = corners(u, v, th)
    pts = [(u, v)] + [(a[0] + f * (b[0] - a[0]), a[1] + f * (b[1] - a[1]))
                      for a, b in zip(cs, cs[1:] + cs[:1]) for f in (0, 0.25, 0.5, 0.75)]
    if not all(in_field(x, y) for x, y in pts):
      return False
    ci, cj = int(u // CELL), int(v // CELL)
    sq = (u, v, th)
    for a in (ci - 1, ci, ci + 1):
      for b in (cj - 1, cj, cj + 1):
        for p in self.grid.get((a, b), ()):
          d = math.hypot(p[0] - u, p[1] - v)
          if d < REACH and (d < SIDE + GAP or gap(p, sq) < GAP - 1e-9):
            return False
    self.squares.append(sq)
    self.grid.setdefault((ci, cj), []).append(sq)
    return True


def default_jitter(tt):
  return (1 - tt) * math.pi / 4


def accept(tt):
  return min(1.0, max(0.0, (tt - 0.15) / 0.6))


def squares_from_field(t, angle, seed, jitter=default_jitter):
  """Squares on tone t and wanted rotation angle (radians), both on the face
  grid. Each 8 mm block lays a brick lattice of pitch PITCH at the angle of
  its centre, over the whole face, so blocks of equal angle share one
  lattice; candidates nearest a lattice node go first. Each node draws one
  coin against clip((t - 0.15) / 0.6), so a refused node stays empty.
  Rotation is the local angle plus up to jitter(t) either way."""
  rng = random.Random(seed)
  blocks = {}
  cand = []
  step = 0.35
  for i in range(int(W / S / step)):
    for j in range(int(H / S / step)):
      u, v = (i + 0.5) * step + rng.uniform(-0.1, 0.1), (j + 0.5) * step + rng.uniform(-0.1, 0.1)
      jj, ii = min(H - 1, int(v * S)), min(W - 1, int(u * S))
      if not ALLOWED_IN[jj, ii] or t[jj, ii] <= 0.15:
        continue
      key = (int(u // BLOCK), int(v // BLOCK))
      if key not in blocks:
        bj = min(H - 1, int((key[1] + 0.5) * BLOCK * S))
        bi = min(W - 1, int((key[0] + 0.5) * BLOCK * S))
        blocks[key] = angle[bj, bi]
      th, pitch = blocks[key], PITCH
      x = u * math.cos(th) + v * math.sin(th)
      y = -u * math.sin(th) + v * math.cos(th)
      row = round(y / pitch)
      x -= 0.5 * pitch * (row % 2)
      col = round(x / pitch)
      d = math.hypot(x - col * pitch, y - row * pitch)
      cand.append((d + 1e-3 * rng.random(), u, v, jj, ii, key + (row, col)))
  cand.sort()

  place, coin = Placer(), {}
  for _, u, v, jj, ii, node in cand:
    if node not in coin:
      coin[node] = rng.random()
    tt = t[jj, ii]
    if coin[node] >= accept(tt):
      continue
    place.try_add(u, v, angle[jj, ii] + rng.uniform(-1, 1) * jitter(tt))
  return place.squares


def orientation(h, sigma):
  """Gradient direction of h, radians, from the smoothed structure tensor so
  it holds steady on ridge tops and valley floors."""
  gv, gu = np.gradient(h, 1 / S)
  jxx, jyy, jxy = blur(gu * gu, sigma), blur(gv * gv, sigma), blur(gu * gv, sigma)
  return 0.5 * np.arctan2(2 * jxy, jxx - jyy)


# --- K0: frost --------------------------------------------------------------

def k0():
  """I0's stuck cells in growth order, each square turned toward the cell it
  stuck to: the earliest stuck lattice neighbour, or the nearest blocked
  ground for a cell that stuck to a seed."""
  cells = i0_list()
  pos = np.array([(u, v) for u, v, _ in cells])
  _, (nj, ni) = distance_transform_edt(ALLOWED, return_indices=True)
  near = cKDTree(pos).query_ball_point(pos, 2.1)
  place = Placer()
  for k, (u, v) in enumerate(pos):
    older = [n for n in near[k] if n < k]
    if older:
      pu, pv = pos[min(older)]
    else:
      j, i = int(v * S), int(u * S)
      pu, pv = (ni[j, i] + 0.5) / S, (nj[j, i] + 0.5) / S
    place.try_add(u, v, math.atan2(pv - v, pu - u))
  return place.squares, None


# --- K1: coral stripes ------------------------------------------------------

def k1():
  """I1's reaction field; squares turned along the stripes."""
  b = zoom(gray_scott(12000, 0.055, 0.062, 211, 195, 248, seeds=24), 7.5, order=1)
  b = (b - b.min()) / (b.max() - b.min())
  t = np.zeros((H, W))
  t[30:30 + b.shape[0], 30:30 + b.shape[1]] = b
  return squares_from_field(t, orientation(t, 1.5) + math.pi / 2, 501), t


# --- K2: sunflower spirals --------------------------------------------------

def k2():
  """I2's whorls and seed order; each seed square turned along the spiral,
  perpendicular to the radius from its whorl centre, kept with the tissue
  tone's acceptance and dropped when it breaks the gap."""
  h, t = tissue()
  hb = blur(h, 4.0)
  rect = (U >= 3.0) & (U <= 189.0) & (V >= 3.0) & (V <= 149.0)
  js, is_ = np.nonzero((maximum_filter(hb, size=5 * S) == hb) & rect)
  centres = []
  for k in np.argsort(-hb[js, is_]):
    u, v = (is_[k] + 0.5) / S, (js[k] + 0.5) / S
    if all(math.hypot(u - cu, v - cv) >= 25.0 for cu, cv in centres):
      centres.append((u, v))
  while len(centres) < 25:
    far = np.full((H, W), np.inf)
    for cu, cv in centres:
      np.minimum(far, np.hypot(U - cu, V - cv), out=far)
    far[~ALLOWED] = 0
    j, i = np.unravel_index(far.argmax(), far.shape)
    if far[j, i] < 25.0:
      break
    centres.append(((i + 0.5) / S, (j + 0.5) / S))

  rng = random.Random(502)
  place = Placer()
  misses = [0] * len(centres)
  active = list(range(len(centres)))
  k = 0
  while active:
    k += 1
    rad, ang = 1.45 * math.sqrt(k), math.radians(k * 137.508)
    for c in list(active):
      u, v = centres[c][0] + rad * math.cos(ang), centres[c][1] + rad * math.sin(ang)
      if 0 <= u < W / S and 0 <= v < H / S and rng.random() < accept(t[int(v * S), int(u * S)]):
        misses[c] = 0 if place.try_add(u, v, ang + math.pi / 2) else misses[c] + 1
      if misses[c] > 80 or rad > 80:
        active.remove(c)
  return place.squares, t


# --- K3: order and disorder -------------------------------------------------

def k3():
  """C12's lit tissue; squares sit square to the face on the crowns and turn
  fully random by tone 0.3."""
  _, t = tissue()
  return squares_from_field(t, np.zeros((H, W)), 503,
                            lambda tt: math.pi / 4 * min(1.0, max(0.0, (0.7 - tt) / 0.4))), t


# --- K4: body folds ---------------------------------------------------------

def k4():
  """I4's ridges and creases; squares turned along the contours."""
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
  t = light_tone(h)
  return squares_from_field(t, orientation(h, 2.0) + math.pi / 2, 504), t


# --- K5: tissue -------------------------------------------------------------

def k5():
  """C12's tissue; squares turned along each facet's slope."""
  h, t = tissue()
  return squares_from_field(t, orientation(h, 0.5), 505), t


# --- Report -----------------------------------------------------------------

CANDIDATES = [
  ('K0', 'I0', k0, 'I0 frost in uniform squares turned toward the cell each stuck to'),
  ('K1', 'I1', k1, 'I1 coral stripes in uniform squares turned along the stripes'),
  ('K2', 'I2', k2, 'I2 sunflower whorls in uniform squares turned along the spirals'),
  ('K3', 'C12', k3, 'C12 tissue as order and disorder: aligned crowns, random shadows'),
  ('K4', 'I4', k4, 'I4 body folds in uniform squares lying along the contours'),
  ('K5', 'C12', k5, 'C12 tissue in uniform squares turned along each facet slope'),
]


def draw(squares):
  img = Image.new('L', (W, H), 0)
  pen = ImageDraw.Draw(img)
  for u, v, th in squares:
    pen.polygon([(x * S - 0.5, y * S - 0.5) for x, y in corners(u, v, th)], fill=255)
  return np.asarray(img) > 0


def min_gap(squares):
  q = np.array(squares)
  pairs = cKDTree(q[:, :2]).query_pairs(REACH, output_type='ndarray')
  return min((gap(squares[a], squares[b]) for a, b in pairs), default=math.inf)


def main(names):
  OUT.mkdir(parents=True, exist_ok=True)
  field = np.count_nonzero(ALLOWED)
  for name, source, fn, what in [c for c in CANDIDATES if not names or c[0] in names]:
    t0 = time.time()
    squares, t = fn()
    light = draw(squares)
    outside = np.count_nonzero(light & ~ALLOWED)
    start = light & ALLOWED
    changed = np.count_nonzero((binary_closing(start, np.ones((6, 6), bool)) & ALLOWED) != start)
    changed /= np.count_nonzero(start)
    path = OUT / f'{name}.png'
    face_image(start).save(path)
    md5 = subprocess.run(['md5', '-q', str(path)], capture_output=True, text=True,
                         check=True).stdout.strip()
    crown = 'n/a' if t is None else (
      f'{100 * np.count_nonzero(start & (t > 0.7)) / np.count_nonzero(ALLOWED & (t > 0.7)):.1f} %')
    g = min_gap(squares)
    print(f'{name} (from {source}): {len(squares)} squares, coverage '
          f'{100 * np.count_nonzero(start) / field:.1f} %, crown {crown}, min gap {g:.3f} mm, '
          f'changed {changed * 100:.2f} %, md5 {md5}, {time.time() - t0:.1f} s', flush=True)
    if g < GAP - 1e-6:
      print(f'  {name} gap rule: min gap {g:.3f} mm', flush=True)
    if outside:
      print(f'  {name} field rule: {outside} light px outside ALLOWED', flush=True)
    if changed > 0:
      disc = np.count_nonzero((closing(start, CLOSE_DISC) & ALLOWED) != start) / np.count_nonzero(start)
      print(f'  {name} print pass: 6x6 square closing changed {changed * 100:.2f} %, '
            f'6 px disc closing {disc * 100:.2f} %; image saved unclosed', flush=True)
    if not 1500 <= len(squares) <= 6000:
      print(f'  {name} count rule: {len(squares)} squares', flush=True)
    if t is not None and float(crown[:-2]) < 30:
      print(f'  {name} crown rule: {crown}', flush=True)
    print(f'  {name}: {what}.', flush=True)


if __name__ == '__main__':
  main(sys.argv[1:])
