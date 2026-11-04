#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family M: on/off squares on one grid.

One invisible grid of 2.0 mm cells covers the face, a cell centre on the hole
column at x = 8.5, y = -85 mm, so the columns run through the slots. Each
cell is either on, a light 1.3 mm square centred in it, or off. The picture
comes only from which cells are on. The patterns come from I0 to I4
(candidates_i.py) and C12's tissue; the face grid, the printability pass and
the output from candidates_b.py.

  candidates_m.py           every candidate's face PNG; prints the report lines
  candidates_m.py M0 M3     only the named candidates
"""

import math
import random
import subprocess
import sys

import numpy as np
from scipy.ndimage import distance_transform_edt, maximum_filter, zoom

from candidates_b import ALLOWED, H, OUT, S, U, V, W, blur, face_image, printable
from candidates_c import fit
from candidates_g import stamp
from candidates_i import gray_scott, light_tone, smin, tissue

PITCH = 2.0          # cell pitch, mm
SIDE = 1.3           # light square, mm; 0.7 mm dark between neighbours

# Cell (m, k) has its centre at u = 2k, v = 0.5 + 2m mm, the hole column
# x = 8.5 mm landing on k = 6. Its 13 px square starts at pixel row 20m - 2,
# column 20k - 6, within 0.05 mm of centred.

ROWS, COLS = 76, 97
J0 = 20 * np.arange(ROWS) - 2
I0 = 20 * np.arange(COLS) - 6
N = round(SIDE * S)
ALLOWED_IN = distance_transform_edt(ALLOWED) > 0.15 * S


def cells_allowed():
  ok = np.zeros((ROWS, COLS), bool)
  for m, j in enumerate(J0):
    for k, i in enumerate(I0):
      if 0 <= j and j + N <= H and 0 <= i and i + N <= W:
        ok[m, k] = ALLOWED_IN[j:j + N, i:i + N].all()
  return ok


CELL_OK = cells_allowed()


def cell_tone(t):
  """Mean of t over each cell's square; 0 for cells off the face."""
  c = np.zeros((H + 1, W + 1))
  c[1:, 1:] = t.cumsum(0).cumsum(1)
  out = np.zeros((ROWS, COLS))
  for m, j in enumerate(J0):
    for k, i in enumerate(I0):
      if 0 <= j and j + N <= H and 0 <= i and i + N <= W:
        out[m, k] = (c[j + N, i + N] - c[j, i + N] - c[j + N, i] + c[j, i]) / (N * N)
  return out


def remap(ct):
  """Shadows to 0, crowns to nearly 1."""
  return np.clip((ct - 0.12) / 0.70, 0, 1)


def diffuse(ct, seed):
  """Serpentine Floyd-Steinberg on the remapped cell tones. Cells outside
  the field take no error. A threshold jitter of 0.05 either way breaks the
  regular worms the diffusion draws in flat mid tones."""
  rng = random.Random(seed)
  r = remap(ct) * CELL_OK
  on = np.zeros((ROWS, COLS), bool)
  for m in range(ROWS):
    step = 1 if m % 2 == 0 else -1
    for k in (range(COLS) if step == 1 else range(COLS - 1, -1, -1)):
      if not CELL_OK[m, k]:
        continue
      on[m, k] = r[m, k] >= 0.5 + rng.uniform(-0.05, 0.05)
      e = r[m, k] - on[m, k]
      for dm, dk, w in ((0, step, 7), (1, -step, 3), (1, 0, 5), (1, step, 1)):
        mm, kk = m + dm, k + dk
        if 0 <= mm < ROWS and 0 <= kk < COLS and CELL_OK[mm, kk]:
          r[mm, kk] += e * w / 16
  return on


BAYER = (np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) + 0.5) / 16


def ordered(ct):
  """Remapped cell tones thresholded by a 4 x 4 Bayer matrix."""
  thr = np.tile(BAYER, (ROWS // 4 + 1, COLS // 4 + 1))[:ROWS, :COLS]
  return (remap(ct) > thr) & CELL_OK


def draw(on):
  light = np.zeros((H, W), bool)
  for m, k in zip(*np.nonzero(on)):
    light[J0[m]:J0[m] + N, I0[k]:I0[k] + N] = True
  return light


def draw_circles(on):
  """Each on cell as a circle on the cell centre, at a radius the print
  opening leaves whole."""
  light = np.zeros((H, W), bool)
  r = fit(0.65)
  for m, k in zip(*np.nonzero(on)):
    stamp(light, PITCH * k, 0.5 + PITCH * m, r)
  return light


# --- M0: frost --------------------------------------------------------------

def m0():
  """Diffusion-limited aggregation on the cell lattice, 4 neighbours. Seeds
  are the blocked cells touching the field; walkers start as far from the
  growth as the field allows, and growth stops while gaps three cells wide
  remain."""
  rng = random.Random(601)
  free = CELL_OK.ravel()
  nb = []
  for c in range(ROWS * COLS):
    m, k = divmod(c, COLS)
    nb.append([mm * COLS + kk for mm, kk in ((m - 1, k), (m + 1, k), (m, k - 1), (m, k + 1))
               if 0 <= mm < ROWS and 0 <= kk < COLS])
  touch = [free[c] and (len(nb[c]) < 4 or not all(free[n] for n in nb[c])) for c in range(len(nb))]
  stuck = [False] * len(nb)
  dist = [0 if not free[c] else 1 if touch[c] else 99 for c in range(len(nb))]

  def spread(front):
    while front:
      nxt = []
      for c in front:
        for n in nb[c]:
          if free[n] and dist[c] + 1 < dist[n]:
            dist[n] = dist[c] + 1
            nxt.append(n)
      front = nxt

  spread([c for c in range(len(nb)) if dist[c] == 1])
  open_cells = [c for c in range(len(nb)) if dist[c] >= 2]
  count, reach, misses = 0, 8, 0
  while count < 3000 and open_cells and reach >= 3:
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
      c = nb[c][int(rng.random() * 4)]
    stuck[c], touch[c], dist[c] = True, False, 0
    count += 1
    for n in nb[c]:
      if free[n] and not stuck[n]:
        touch[n] = True
    spread([c])
  return np.array(stuck).reshape(ROWS, COLS)


# --- M1: coral stripes ------------------------------------------------------

def m1():
  b = zoom(gray_scott(12000, 0.055, 0.062, 211, 195, 248, seeds=24), 7.5, order=1)
  b = (b - b.min()) / (b.max() - b.min())
  t = np.zeros((H, W))
  t[30:30 + b.shape[0], 30:30 + b.shape[1]] = b
  return diffuse(cell_tone(t), 611)


# --- M2: sunflower spirals --------------------------------------------------

def m2():
  """I2's whorl centres and golden-angle seeds. Each seed turns on the cell
  it falls in, gated by the remapped tissue tone; a seed whose cell is
  already on or outside the field is skipped and counts toward its whorl's
  stop."""
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

  # A seed next to a cell of another whorl is skipped too, so each whorl
  # stops at a one-cell dark seam instead of filling its neighbours' gaps.

  rng = random.Random(612)
  on = np.zeros((ROWS, COLS), bool)
  owner = np.full((ROWS + 2, COLS + 2), -1)
  misses = [0] * len(centres)
  active = list(range(len(centres)))
  k = 0
  while active:
    k += 1
    rad, ang = 1.45 * math.sqrt(k), math.radians(k * 137.508)
    for c in list(active):
      u, v = centres[c][0] + rad * math.cos(ang), centres[c][1] + rad * math.sin(ang)
      cm, ck = round((v - 0.5) / PITCH), round(u / PITCH)
      if 0 <= cm < ROWS and 0 <= ck < COLS:
        near = owner[cm:cm + 3, ck:ck + 3]
        if not CELL_OK[cm, ck] or on[cm, ck] or ((near >= 0) & (near != c)).any():
          misses[c] += 1
        elif rng.random() < remap(t[min(H - 1, int(v * S)), min(W - 1, int(u * S))]):
          on[cm, ck] = True
          owner[cm + 1, ck + 1] = c
          misses[c] = 0
      if misses[c] > 80 or rad > 80:
        active.remove(c)
  return on


# --- M3 and M5: tissue ------------------------------------------------------

def m3():
  return ordered(cell_tone(tissue()[1]))


def m5():
  return diffuse(cell_tone(tissue()[1]), 615)


# --- M4: body folds ---------------------------------------------------------

def m4():
  """I4's ridges and creases, lit as before."""
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
  return diffuse(cell_tone(light_tone(blur(h, 1.5))), 614)


# --- Report -----------------------------------------------------------------

CANDIDATES = [
  ('M0', 'I0', m0, 'frost grown cell by cell on the grid'),
  ('M1', 'I1', m1, 'coral stripes diffused to on and off cells'),
  ('M2', 'I2', m2, 'sunflower whorls, each seed lighting the cell it falls in'),
  ('M3', 'C12', m3, 'C12 tissue as a regular 4 x 4 Bayer screen'),
  ('M4', 'I4', m4, 'body folds diffused to on and off cells'),
  ('M5', 'C12', m5, 'C12 tissue diffused to on and off cells'),
  ('M5a', 'C12', m5, "M5's on cells drawn as circles"),
]

DRAW = {'M5a': draw_circles}     # candidates drawn other than as squares


def main(names):
  OUT.mkdir(parents=True, exist_ok=True)
  field = np.count_nonzero(ALLOWED)
  for name, source, fn, what in [c for c in CANDIDATES if not names or c[0] in names]:
    on = fn()
    assert not (on & ~CELL_OK).any()

    # The print pass's disc opening trims each square's corner pixels, so
    # its share is reported and the exact marks are saved.

    light = DRAW.get(name, draw)(on) & ALLOWED
    _, changed = printable(light)
    face_image(light).save(OUT / f'{name}.png')
    lit_px = np.count_nonzero(light)
    md5 = subprocess.run(['md5', '-q', str(OUT / f'{name}.png')], capture_output=True, text=True,
                         check=True).stdout.strip()
    print(f'{name} (from {source}): {np.count_nonzero(on)} cells on of {np.count_nonzero(CELL_OK)} '
          f'allowed, coverage {100 * lit_px / field:.1f} %, changed {changed * 100:.2f} %, '
          f'md5 {md5}', flush=True)
    if changed > 0:
      print(f'  {name} print pass: changed {changed * 100:.2f} %', flush=True)
    if name == 'M0' and not 1500 <= np.count_nonzero(on) <= 3000:
      print(f'  {name} count rule: {np.count_nonzero(on)} cells on', flush=True)
    print(f'  {name}: {what}.', flush=True)


if __name__ == '__main__':
  main(sys.argv[1:])
