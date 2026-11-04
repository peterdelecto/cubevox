#!/usr/bin/env python3
"""Owner floral art as a 45 degree round-dot halftone for the lid-face inlay.

Dot area follows local coverage of the raw art mask. Under 50 % coverage the light
shows as dots on the lattice. Over 50 % the dots merge and the dark shows as holes
on the offset lattice, with hole area matching the remaining dark share.

  python3 -I candidates_y.py art.ai [Y0 Y1 Y2]   writes build/cane/candidates/<name>.png
"""
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np

import art_inlay
import candidates_b as b

PITCHES = {'Y0': 1.6, 'Y1': 2.0, 'Y2': 2.4}
MIN_DOT = 0.8     # mm, smallest light dot kept
MIN_HOLE = 0.6    # mm, smallest dark hole kept
NONE_BELOW = 0.04 # coverage under this gets no dot


def halftone(raw, pitch):
  cov = b.blur(raw.astype(np.float32), pitch / 2)
  yy, xx = np.mgrid[0:b.H, 0:b.W].astype(np.float32)
  c45 = s45 = np.float32(np.sqrt(0.5))
  p = pitch * b.S
  a = (xx * c45 + yy * s45) / p
  v = (-xx * s45 + yy * c45) / p

  def sample(ia, ib):
    """Coverage at lattice node (ia, ib) and the pixel distance to it, in mm."""
    cx = (ia * c45 - ib * s45) * p
    cy = (ia * s45 + ib * c45) * p
    ix = np.clip(np.rint(cx).astype(int), 0, b.W - 1)
    iy = np.clip(np.rint(cy).astype(int), 0, b.H - 1)
    return cov[iy, ix], np.hypot(xx - cx, yy - cy) / b.S

  c0, d0 = sample(np.rint(a), np.rint(v))
  c1, d1 = sample(np.floor(a) + 0.5, np.floor(v) + 0.5)

  r_dot = np.sqrt(np.clip(c0, 0, 1) / np.pi) * pitch
  dots = (c0 < 0.5) & (c0 >= NONE_BELOW) & (2 * r_dot >= MIN_DOT) & (d0 < r_dot)

  r_hole = np.sqrt(np.clip(1 - c1, 0, 1) / np.pi) * pitch
  holes = (2 * r_hole >= MIN_HOLE) & (d1 < r_hole)
  merged = (c1 >= 0.5) & ~holes
  # near-solid cells whose holes would print too small stay solid
  return (dots | merged) & b.ALLOWED


def main(path, names):
  b.OUT.mkdir(parents=True, exist_ok=True)
  raw = art_inlay.face_light(art_inlay.art_raster(path, 146 / 2000))
  for name in names:
    light, changed = b.printable(halftone(raw, PITCHES[name]))
    out = b.OUT / f'{name}.png'
    b.face_image(light).save(out)
    share = light.sum() / b.ALLOWED.sum()
    md5 = hashlib.md5(out.read_bytes()).hexdigest()
    print(f'{name}: pitch {PITCHES[name]} mm, light {share * 100:.1f} %, '
          f'print pass changed {changed * 100:.1f} %, md5 {md5}')


if __name__ == '__main__':
  main(sys.argv[1], sys.argv[2:] or list(PITCHES))
