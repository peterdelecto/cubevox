#!/usr/bin/env python3
"""Owner art (a flat two-colour EPS) as a printable lid-face inlay mask.

The art's coloured shapes become the inlay; its background stays lid. The art is
centred on the field at K mm per art point and repeats if smaller than the field.

  python3 -I art_inlay.py art.eps [k_mm_per_pt] [--lines]   writes build/cane/art/<stem>.png

--lines is for outline drawings: lines grow evenly instead of hatching filling in.
"""
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np
import pymupdf

import candidates_b as b
import eps_art

ART_OUT = b.ROOT / 'build' / 'cane' / 'art'


def art_raster(path, k):
  """Coloured-shape mask of the art at the face grid's 10 px/mm, row 0 at the
  art's top edge. The background is the lightest fill colour."""
  fills = eps_art.parse(path)
  ground = max({rgb for rgb, _, _ in fills}, key=sum)
  shapes = [((0, 0, 0), sps, eo) for rgb, sps, eo in fills if rgb != ground]
  with tempfile.TemporaryDirectory() as tmp:
    svg = Path(tmp) / 'art.svg'
    eps_art.write_svg(shapes, svg, *eps_art.size(path))
    z = b.S * k
    pix = pymupdf.open(svg)[0].get_pixmap(matrix=pymupdf.Matrix(z, z),
                                          colorspace=pymupdf.csGRAY, alpha=False)
  a = np.frombuffer(pix.samples, np.uint8).reshape(pix.height, pix.width)
  return a < 128


def face_light(art):
  """The art centred on the field, repeated to cover it, clipped to ALLOWED."""
  ah, aw = art.shape
  cu, cv = b.to_uv((b.FIELD[0] + b.FIELD[2]) / 2, (b.FIELD[1] + b.FIELD[3]) / 2)
  rows = (np.arange(b.H) - round(cv * b.S) + ah // 2) % ah
  cols = (np.arange(b.W) - round(cu * b.S) + aw // 2) % aw
  return art[np.ix_(rows, cols)] & b.ALLOWED


def thicken(m):
  """Engraving lines sit far under the print floors. Gaps under 0.6 mm fill, so
  hatched petals go solid; strokes under 0.8 mm grow to 0.9 mm rather than vanish."""
  m = b.closing(m, b.CLOSE_DISC)
  thin = m & ~b.opening(m, b.OPEN_DISC)
  return (m | b.dilate(thin, b.disc(7))) & b.ALLOWED


def thicken_lines(m):
  """Line art: every line grows 0.4 mm, so 0.4 mm lines reach the 0.8 mm floor
  and close-running lines stay apart wherever the gap allows."""
  return b.dilate(m, b.disc(4)) & b.ALLOWED


def main(path, k, lines=False):
  ART_OUT.mkdir(parents=True, exist_ok=True)
  raw = face_light(art_raster(path, k))
  light, _ = b.printable(thicken_lines(raw) if lines else thicken(raw))
  changed = np.count_nonzero(light != raw) / np.count_nonzero(raw)
  stem = Path(path).stem
  b.face_image(raw).save(ART_OUT / f'{stem}-raw.png')
  b.face_image(light).save(ART_OUT / f'{stem}.png')
  np.save(ART_OUT / f'{stem.lower()}.npy', light)
  share = light.sum() / b.ALLOWED.sum()
  print(f'k {k} mm/pt, light {share * 100:.1f} %, print pass changed {changed * 100:.1f} %')


if __name__ == '__main__':
  # Default scale: the art's height fills the field's 146 mm.

  args = [s for s in sys.argv[1:] if s != '--lines']
  k = float(args[1]) if len(args) > 1 else 146 / eps_art.size(args[0])[1]
  main(args[0], k, '--lines' in sys.argv)
