#!/usr/bin/env python3
"""Owner photo of a plaster relief as a flat two-colour lid-face inlay.

The photo is lit from above, so each raised shape casts a shadow along its lower
edge. Those shadows become the dark inlay, and the face reads as raised plaster
under a low light. The photo's height spans the field, centred; the pattern runs
to EDGE_CLEAR from every hole and the window recess.

  python3 -I photo_inlay.py photo.jpg [threshold] [blur_px]   writes build/cane/art/<stem>.png
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np
from PIL import Image
from scipy import ndimage as ndi

import candidates_b as b

ART_OUT = b.ROOT / 'build' / 'cane' / 'art'
REGION = b.ALLOWED_TO_EDGES

LIGHTING_PX = 80        # blur that takes out the photo's uneven lighting
SHAPE_PX = 25           # blur that the relief's shading is measured against
LID, INLAY = (236, 232, 222), (70, 66, 60)


def shading(path):
  """Relief shading at 10 px/mm, the photo's height on the field's: positive on
  lit slopes, negative in shadow."""
  im = Image.open(path).convert('L')
  rows = round((b.FIELD[3] - b.FIELD[1]) * b.S)
  cols = round(im.width * rows / im.height)
  g = np.asarray(im.resize((cols, rows), Image.Resampling.LANCZOS), np.float32)
  flat = g / ndi.gaussian_filter(g, LIGHTING_PX)
  return (flat - ndi.gaussian_filter(flat, SHAPE_PX)) * 230


def on_face(m):
  """The photo mask centred on the field, cropped to the face grid."""
  face = np.zeros((b.H, b.W), bool)
  cu, cv = b.to_uv((b.FIELD[0] + b.FIELD[2]) / 2, (b.FIELD[1] + b.FIELD[3]) / 2)
  r0, c0 = round(cv * b.S) - m.shape[0] // 2, round(cu * b.S) - m.shape[1] // 2
  a0, a1 = max(r0, 0), min(r0 + m.shape[0], b.H)
  e0, e1 = max(c0, 0), min(c0 + m.shape[1], b.W)
  face[a0:a1, e0:e1] = m[a0 - r0:a1 - r0, e0 - c0:e1 - c0]
  return face


def thicken(m):
  """Grain specks go; gaps under 0.6 mm fill; every stroke grows 0.2 mm a side,
  evenly, so 0.4 mm shadows reach the 0.8 mm floor with smooth edges."""
  m = b.closing(b.opening(m, b.disc(3)), b.CLOSE_DISC)
  return b.dilate(m, b.disc(5)) & REGION


def proof(inlay):
  img = np.empty((b.H, b.W, 3), np.uint8)
  img[:] = LID
  img[inlay] = INLAY
  img[b.VOID_MASK] = b.VOID
  return Image.fromarray(img)


def main(path, threshold, blur):
  ART_OUT.mkdir(parents=True, exist_ok=True)
  raw = on_face(ndi.gaussian_filter(shading(path), blur) < threshold) & REGION
  inlay, _ = b.printable(thicken(raw), REGION)
  changed = np.count_nonzero(inlay != raw) / np.count_nonzero(raw)
  stem = Path(path).stem
  proof(raw).save(ART_OUT / f'{stem}-raw.png')
  img = proof(inlay)
  img.save(ART_OUT / f'{stem}.png')
  img.crop((700, 700, 1300, 1150)).resize((1200, 900), Image.Resampling.NEAREST).save(
    ART_OUT / f'{stem}-zoom.png')
  np.save(ART_OUT / f'{stem.lower()}.npy', inlay)
  share = inlay.sum() / REGION.sum()
  print(f'threshold {threshold}, blur {blur} px, inlay {share * 100:.1f} %, '
        f'print pass changed {changed * 100:.1f} %')


if __name__ == '__main__':
  a = sys.argv[1:]
  main(a[0], float(a[1]) if len(a) > 1 else -4.0, float(a[2]) if len(a) > 2 else 1.5)
