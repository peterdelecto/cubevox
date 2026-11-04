#!/usr/bin/env python3
"""Cane webbing inlay for the lid face: one repeat as convex polygons.

Paired horizontal and vertical strands sit on the band lines x = jP, y = kP.
Single diagonals run through the band midpoints and cut the corners of the
octagonal holes. A strand that passes under another stops `gap` short on each
side of it, so the dark lid shows the over-and-under.

  weave.py tile  X0 Y0          one repeat at (X0, Y0) as JSON, in mm
  weave.py proof out.png        the whole face field as a PNG proof
"""

import json
import math
import sys

PITCH = 12.0      # repeat, mm
STRAND = 2.5      # width of each strand of a pair
PAIR_GAP = 0.6    # gap between the two strands of a pair
DIAG = 2.2        # width of a diagonal strand
GAP = 0.6         # gap each side of an over strand
MIN_WIDTH = 0.8   # narrower pieces are dropped; the nozzle cannot draw them

OFFSET = (STRAND + PAIR_GAP) / 2


def strands(lo, hi):
  """Every strand whose band crosses the square [lo, hi]^2."""
  out = []
  j0, j1 = math.floor(lo / PITCH) - 1, math.ceil(hi / PITCH) + 1
  for j in range(j0, j1 + 1):
    for side in (-1, 1):
      out.append(dict(kind='v', n=(1.0, 0.0), c=j * PITCH + side * OFFSET,
                      h=STRAND / 2, side=side))
      out.append(dict(kind='h', n=(0.0, 1.0), c=j * PITCH + side * OFFSET,
                      h=STRAND / 2, side=side))
  r = math.sqrt(0.5)
  for a in range(math.floor((lo - hi) / PITCH) - 1, math.ceil(2 * hi / PITCH) + 1):
    c = (a + 0.5) * PITCH * r
    out.append(dict(kind='/', n=(-r, r), c=c, h=DIAG / 2))   # y - x = (a + 1/2) P
    out.append(dict(kind='\\', n=(r, r), c=c, h=DIAG / 2))   # y + x = (a + 1/2) P
  return out


def crossing(a, b):
  (ax, ay), (bx, by) = a['n'], b['n']
  det = ax * by - ay * bx
  if abs(det) < 1e-9:
    return None
  return ((a['c'] * by - ay * b['c']) / det, (ax * b['c'] - a['c'] * bx) / det)


def on_band(v):
  m = v % PITCH
  return min(m, PITCH - m) < 1e-6


def over(b, a, p):
  """True when strand b lies over strand a at their crossing p."""
  pair = ('h', 'v')
  if a['kind'] in pair and b['kind'] not in pair:
    return True
  if a['kind'] not in pair and b['kind'] in pair:
    return False
  if a['kind'] in pair:

    # Horizontal pairs lie on top at even junctions, vertical pairs at odd ones.

    h_over = (round(p[0] / PITCH) + round(p[1] / PITCH)) % 2 == 0
    return h_over if b['kind'] == 'h' else not h_over
  back_over = on_band(p[0])   # on a vertical band "\" is on top
  return back_over if b['kind'] == '\\' else not back_over


def clip(poly, n, c, keep_below):
  """Keep the part of a convex polygon with n.p <= c (or >= c)."""
  def inside(p):
    d = n[0] * p[0] + n[1] * p[1] - c
    return d <= 1e-9 if keep_below else d >= -1e-9
  out = []
  for i, p in enumerate(poly):
    q = poly[(i + 1) % len(poly)]
    pin, qin = inside(p), inside(q)
    if pin:
      out.append(p)
    if pin != qin:
      dp = n[0] * p[0] + n[1] * p[1] - c
      dq = n[0] * q[0] + n[1] * q[1] - c
      t = dp / (dp - dq)
      out.append((p[0] + t * (q[0] - p[0]), p[1] + t * (q[1] - p[1])))
  return out


def area(poly):
  return 0.5 * abs(sum(p[0] * q[1] - q[0] * p[1]
                       for p, q in zip(poly, poly[1:] + poly[:1])))


def width(poly):
  """Smallest extent of a convex polygon across any of its edges."""
  best = float('inf')
  for p, q in zip(poly, poly[1:] + poly[:1]):
    ex, ey = q[0] - p[0], q[1] - p[1]
    length = math.hypot(ex, ey)
    if length < 1e-9:
      continue
    nx, ny = -ey / length, ex / length
    best = min(best, max(abs((v[0] - p[0]) * nx + (v[1] - p[1]) * ny)
                         for v in poly))
  return best


def pieces(lo, hi):
  """Strand pieces inside [lo, hi]^2, weave coordinates (a junction at 0, 0)."""
  box = [(lo, lo), (hi, lo), (hi, hi), (lo, hi)]
  all_strands = strands(lo, hi)
  out = []
  for a in all_strands:
    poly = clip(clip(box, a['n'], a['c'] + a['h'], True), a['n'],
                a['c'] - a['h'], False)
    parts = [poly] if len(poly) > 2 else []
    for b in all_strands:
      p = crossing(a, b)
      if p is None or not over(b, a, p):
        continue
      lo_c, hi_c = b['c'] - b['h'] - GAP, b['c'] + b['h'] + GAP
      split = []
      for q in parts:
        split += [clip(q, b['n'], lo_c, True), clip(q, b['n'], hi_c, False)]
      parts = [q for q in split if len(q) > 2 and area(q) > 1e-4]
    out += [q for q in parts if width(q) >= MIN_WIDTH]
  return out


def tile(x0, y0):
  """One repeat (two band pitches square, since the pair crossings alternate
  junction by junction), moved so its corner sits at (x0, y0)."""
  out = []
  size = 2 * PITCH
  for poly in pieces(-PITCH, 3 * PITCH):
    for n, c, below in (((1, 0), 0, False), ((1, 0), size, True),
                        ((0, 1), 0, False), ((0, 1), size, True)):
      poly = clip(poly, n, c, below)
      if len(poly) < 3:
        break
    if len(poly) > 2 and area(poly) > 0.01:
      out.append([(round(x0 + x, 4), round(y0 + y, 4)) for x, y in poly])
  return out


def proof(path):
  """Face field (lid face coordinates, mm) drawn at 10 px/mm."""
  from PIL import Image, ImageDraw
  band_x, band_y = 2.5, 5.0                 # band lines at 2.5 + 12k, 5 + 12k
  face = (-3.5, -148.5, 188.5, 3.5)
  field = (-0.5, -145.5, 185.5, 0.5)
  holes = ([(8.5 + 24 * i, -85.0, 6.25) for i in range(8)]
           + [(8.5 + 24 * i, -108.5, 8.8) for i in range(8)]
           + [(8.5 + 24 * i, -134.5, 8.8) for i in range(8)]
           + [(8.5, -32.0, 6.25), (176.5, -31.5, 8.8), (117.94, -40.75, 7.97)])
  oled = (61.535, -54.75, 98.035, -30.0)     # the window (owner 2026-11-03)
  pocket = (60.54, -61.0, 99.04, -24.0)       # the 0.5 mm recess round it
  clear = 1.0
  s = 10
  w, h = int((face[2] - face[0]) * s), int((face[3] - face[1]) * s)
  img = Image.new('RGB', (w, h), (40, 30, 24))
  d = ImageDraw.Draw(img)
  px = lambda x, y: ((x - face[0]) * s, (face[3] - y) * s)
  span = max(face[2] - face[0], face[3] - face[1]) + 2 * PITCH

  # Same junction parity as the tile placed at (-21.5, -151) in Fusion.

  ox, oy = band_x - 2 * PITCH, band_y - 13 * PITCH
  narrow = 0
  for poly in pieces(-PITCH, span):
    if width(poly) < 0.8:
      narrow += 1
    d.polygon([px(x + ox, y + oy) for x, y in poly], fill=(196, 150, 96))
  dark = (40, 30, 24)
  fx0, fy0 = px(field[0], field[3])
  fx1, fy1 = px(field[2], field[1])
  d.rectangle([0, 0, w, fy0], fill=dark)
  d.rectangle([0, fy1, w, h], fill=dark)
  d.rectangle([0, 0, fx0, h], fill=dark)
  d.rectangle([fx1, 0, w, h], fill=dark)
  for x, y, dia in holes:
    r = dia / 2 + clear
    a, b = px(x - r, y + r), px(x + r, y - r)
    d.ellipse([a, b], fill=dark)
    r = dia / 2
    d.ellipse([px(x - r, y + r), px(x + r, y - r)], fill=(255, 255, 255))
  d.rectangle([px(pocket[0] - clear, pocket[3] + clear),
               px(pocket[2] + clear, pocket[1] - clear)], fill=dark)
  d.rectangle([px(oled[0], oled[3]), px(oled[2], oled[1])], fill=(255, 255, 255))
  img.save(path)
  print(f'{path}: {w}x{h} px, {narrow} pieces under 0.8 mm wide before clipping')


if __name__ == '__main__':
  if sys.argv[1] == 'tile':
    print(json.dumps(tile(float(sys.argv[2]), float(sys.argv[3]))))
  else:
    proof(sys.argv[2])
