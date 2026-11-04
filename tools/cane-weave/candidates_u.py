#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family U: camouflage.

U0 is two-colour woodland camouflage. Large lobed patches (blurred noise, sigma
5.0 mm, light above its 55th percentile) carry medium patches (sigma 2.2 mm,
light above its 72nd percentile) combined by XOR, so a medium patch cuts a
dark hole in a large one and shows as a light spot on dark ground. Openings
enforce the 0.8 mm light and 0.6 mm dark minimums, then light components under
2.0 mm2 and dark holes under 1.5 mm2 are removed. The face grid, the
printability pass and the output come from candidates_b.py.

  candidates_u.py           U0 as a face PNG
  candidates_u.py U0        only the named candidates
"""

import sys

import numpy as np
from scipy.ndimage import gaussian_filter, label

from candidates_b import ALLOWED, H, OUT, S, W, disc, opening, render

LARGE_SIGMA = 5.0 * S          # px
LARGE_PCT = 55                 # percentile; light above it
MEDIUM_SIGMA = 2.2 * S         # px
MEDIUM_PCT = 72
LIGHT_DISC = disc(8)           # 0.8 mm light minimum
DARK_DISC = disc(6)            # 0.6 mm dark minimum
MIN_LIGHT_AREA = 2.0 * S * S   # px, 2.0 mm2
MIN_DARK_AREA = 1.5 * S * S    # px, 1.5 mm2


def patches(rng, sigma, pct):
  field = gaussian_filter(rng.standard_normal((H, W)), sigma)
  return field > np.percentile(field, pct)


def drop_small(mask, area):
  """Removes connected components of mask smaller than area px."""
  comp, n = label(mask)
  sizes = np.bincount(comp.ravel(), minlength=n + 1)
  keep = sizes >= area
  keep[0] = False
  return keep[comp]


def camo(seed=71):
  rng = np.random.default_rng(seed)
  large = patches(rng, LARGE_SIGMA, LARGE_PCT)
  medium = patches(rng, MEDIUM_SIGMA, MEDIUM_PCT)
  light = large ^ medium
  light = opening(light, LIGHT_DISC)
  light = ~opening(~light, DARK_DISC)
  light = drop_small(light, MIN_LIGHT_AREA)
  light = ~drop_small(~light, MIN_DARK_AREA)
  return light & ALLOWED


CANDIDATES = [('U0', camo)]


def main(names):
  OUT.mkdir(parents=True, exist_ok=True)
  for name, fn in CANDIDATES:
    if not names or name in names:
      render(name, fn)


if __name__ == '__main__':
  main(sys.argv[1:])
