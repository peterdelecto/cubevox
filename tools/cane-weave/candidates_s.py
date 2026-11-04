#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family S: analogue TV snow.

S0 is a grid of TV pixels, 1.8 mm by 1.5 mm cells, each holding a light
1.2 mm by 0.9 mm rectangle with probability near 0.5. A slow cloud shifts
the density, and one row in twelve has a stretch of denser "tear" pixels.

S0a blurs white noise wider than tall (0.35 mm vertical, 0.7 mm horizontal
sigma) and thresholds it at the median, so about half the field is light grain.
Openings then enforce the 0.8 mm light and 0.6 mm dark print minimums. The
face grid, the printability pass and the output come from candidates_b.py.

  candidates_s.py           S0 as a face PNG
  candidates_s.py S0 S0a    only the named candidates
"""

import sys

import numpy as np
from scipy.ndimage import gaussian_filter

from candidates_b import ALLOWED, H, OUT, S, W, disc, opening, render

SIGMA_V = 3.5                  # vertical blur, px
SIGMA_H = 7.0                  # horizontal blur, px
LIGHT_DISC = disc(8)           # 0.8 mm light minimum
DARK_DISC = disc(6)            # 0.6 mm dark minimum


PITCH_X, PITCH_Y = 1.8, 1.5    # cell pitch, mm
PIX_W, PIX_H = 12, 9           # light rectangle, px, centred in its cell
CLOUD_SIGMA = 12.0             # density cloud blur, mm
TEAR_EVERY = 12                # one tear row per this many rows
TEAR_P = 0.8                   # on-probability inside a tear
TEAR_LEN = (20.0, 60.0)        # tear length, mm


def snow(seed=70):
  rng = np.random.default_rng(seed)
  cols = int(np.ceil(W / S / PITCH_X))
  rows = int(np.ceil(H / S / PITCH_Y))
  cloud = gaussian_filter(rng.standard_normal((rows, cols)),
                          (CLOUD_SIGMA / PITCH_Y, CLOUD_SIGMA / PITCH_X))
  cloud /= cloud.std()
  p = np.clip(0.5 + 0.15 * cloud, 0.3, 0.7)
  for g in range(0, rows, TEAR_EVERY):
    m = int(rng.integers(g, min(g + TEAR_EVERY, rows)))
    length = rng.uniform(*TEAR_LEN)
    x0 = rng.uniform(0, W / S - length)
    k0, k1 = int(np.ceil(x0 / PITCH_X)), int(np.floor((x0 + length) / PITCH_X)) + 1
    p[m, k0:k1] = TEAR_P
  on = rng.random((rows, cols)) < p
  light = np.zeros((H, W), bool)
  ox = round((PITCH_X * S - PIX_W) / 2)
  oy = round((PITCH_Y * S - PIX_H) / 2)
  for m, k in zip(*np.nonzero(on)):
    j, i = round(m * PITCH_Y * S) + oy, round(k * PITCH_X * S) + ox
    light[j:j + PIX_H, i:i + PIX_W] = True
  return light & ALLOWED


def grain(seed=70):
  noise = np.random.default_rng(seed).standard_normal((H, W))
  field = gaussian_filter(noise, (SIGMA_V, SIGMA_H))
  light = field > np.median(field)
  light = opening(light, LIGHT_DISC)
  light = ~opening(~light, DARK_DISC)
  return light & ALLOWED


CANDIDATES = [('S0', snow), ('S0a', grain)]


def main(names):
  OUT.mkdir(parents=True, exist_ok=True)
  for name, fn in CANDIDATES:
    if not names or name in names:
      render(name, fn)


if __name__ == '__main__':
  main(sys.argv[1:])
