#!/usr/bin/env python3
"""Simplified lid-decal masks of the owner's art, in sets of scale and cleanup settings.

  python3 -I art_simple.py art.eps S     S1-S6 (default)
  python3 -I art_simple.py art.eps D     S0 baseline and D1-D4, full detail

Writes build/cane/art/<stem>-<name>.png, <stem lower>-<name lower>.npy and the contact
sheet <stem>-simple-sheet.png (S) or <stem>-detail-sheet.png (D), and prints one table. Narrowest feature is the largest
inscribed-disc diameter of the thinnest traced piece: the distance transform
maximum per piece, smallest over the pieces.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage

import art_inlay
import candidates_b as b
import eps_art
import inlay_export

# name, k multiplier, open disc mm, close disc mm, min piece mm2, simplify mm

SETS = {
  'S': [
    ('S1', 1.0, 1.2, 0.8, 4, 0.15),
    ('S2', 1.0, 1.6, 1.0, 8, 0.20),
    ('S3', 1.5, 1.2, 0.8, 6, 0.15),
    ('S4', 1.5, 1.6, 1.0, 10, 0.20),
    ('S5', 2.0, 1.6, 1.0, 10, 0.20),
    ('S6', 2.0, 2.0, 1.2, 15, 0.25),
  ],

  # S0 is the old pipeline, run for its baseline numbers; it is not on the sheet.

  'D': [
    ('S0', 1.0, 0.8, 0.6, 0.5, 0.03),
    ('D1', 1.0, 0.8, 0.6, 0.5, 0.12),
    ('D2', 1.0, 0.8, 0.6, 2, 0.15),
    ('D3', 1.0, 1.0, 0.8, 2, 0.15),
    ('D4', 1.25, 1.0, 0.8, 3, 0.15),
  ],
}
SHEETS = {'S': ('simple', 3), 'D': ('detail', 2)}     # sheet name, columns


def thicken(m, open_px, close_px):
  """Gaps under the close disc fill. Strokes under the open disc grow by a disc one
  pixel smaller than it, so thin strokes reach the floor instead of vanishing."""
  m = b.closing(m, b.disc(close_px))
  thin = m & ~b.opening(m, b.disc(open_px))
  return (m | b.dilate(thin, b.disc(open_px - 1))) & b.ALLOWED


def printable(light, open_d, close_d, region=b.ALLOWED):
  """b.printable with the candidate's discs."""
  m = light & region
  for _ in range(6):
    nxt = b.closing(b.opening(m, open_d), close_d) & region
    if np.array_equal(nxt, m):
      break
    m = nxt
  while True:
    m = b.opening(m, open_d)
    thin = b.closing(m, close_d) & ~m
    if not thin.any():
      break
    m &= ~b.dilate(thin, close_d)
  assert np.array_equal(b.opening(m, open_d), m)
  return m


def thinnest_piece_mm(mask, doc):
  """Smallest over traced pieces of the piece's widest inscribed disc, mm."""
  lab, n = ndimage.label(mask, structure=np.ones((3, 3)))
  edt = ndimage.distance_transform_edt(np.pad(mask, 1))[1:-1, 1:-1]
  tops = ndimage.maximum(edt, lab, range(1, n + 1))
  area = ndimage.sum(mask, lab, range(1, n + 1)) / b.S ** 2
  keep = [t for t, a in zip(tops, area) if a >= doc['min_loop_mm2']]
  return 2 * min(keep) / b.S if keep else 0.0


def label_font(size):
  for p in ('/System/Library/Fonts/Helvetica.ttc', '/Library/Fonts/Arial.ttf'):
    try:
      return ImageFont.truetype(p, size)
    except OSError:
      pass
  return ImageFont.load_default()


def baseline():
  p = b.ROOT / 'build' / 'cane' / 'adobestock_1435698952-inlay.json'
  if not p.exists():
    return None
  d = json.loads(p.read_text())
  pts = sum(len(q['outer']) + sum(len(h) for h in q['holes']) for q in d['polygons'])
  return d['count'], pts


def main(path, group='S'):
  art_inlay.ART_OUT.mkdir(parents=True, exist_ok=True)
  out = art_inlay.ART_OUT
  stem = Path(path).stem
  k0 = 146 / eps_art.size(path)[1]
  rows, tiles = [], []
  allowed_mm2 = b.ALLOWED.sum() / b.S ** 2
  for name, mult, op, cl, minp, simp in SETS[group]:
    k = k0 * mult
    op_px, cl_px = round(op * b.S), round(cl * b.S)
    open_d, close_d = b.disc(op_px), b.disc(cl_px)
    raw = art_inlay.face_light(art_inlay.art_raster(path, k))
    m = thicken(raw, op_px, cl_px)
    mask = printable(m, open_d, close_d)
    b.face_image(mask).save(out / f'{stem}-{name}.png')
    np.save(out / f'{stem.lower()}-{name.lower()}.npy', mask)
    doc, stats = inlay_export.polygons_doc(mask, simp, minp)
    row = dict(name=name, k=k, op=op, cl=cl, minp=minp, simp=simp, pieces=doc['count'],
               holes=stats['holes'], points=stats['points'],
               light=100 * doc['area'] / allowed_mm2, narrow=thinnest_piece_mm(mask, doc))
    rows.append(row)
    if name != 'S0':
      tiles.append((name, row, b.face_image(mask)))
    print(f'{name} done', file=sys.stderr)

  # Contact sheet.

  sheet_name, cols = SHEETS[group]
  tw = 960
  th = round(tw * b.H / b.W)
  bar = 90
  nrows = -(-len(tiles) // cols)
  sheet = Image.new('RGB', (cols * tw, nrows * (th + bar)), (20, 20, 20))
  d = ImageDraw.Draw(sheet)
  font = label_font(56)
  for i, (name, r, img) in enumerate(tiles):
    x, y = (i % cols) * tw, (i // cols) * (th + bar)
    sheet.paste(img.resize((tw, th), Image.LANCZOS), (x, y + bar))
    d.text((x + 16, y + 14), f"{name}  {r['pieces']} pieces  {r['points']} pts",
           fill=(255, 255, 255), font=font)
  sheet.save(out / f'{stem}-{sheet_name}-sheet.png')

  head = ('name', 'k mm/pt', 'open', 'close', 'min mm2', 'simplify', 'pieces', 'holes',
          'points', 'light %', 'narrow mm')
  print('  '.join(f'{h:>9}' for h in head))
  for r in rows:
    print('  '.join(f'{v:>9}' for v in (
      r['name'], f"{r['k']:.3f}", r['op'], r['cl'], r['minp'], r['simp'], r['pieces'],
      r['holes'], r['points'], f"{r['light']:.1f}", f"{r['narrow']:.2f}")))
  base = baseline()
  if base:
    print(f'baseline (existing json): pieces {base[0]}, points {base[1]}')


if __name__ == '__main__':
  if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] not in SETS):
    raise SystemExit(__doc__)
  main(*sys.argv[1:])
