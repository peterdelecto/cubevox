#!/usr/bin/env python3
"""The cane weave of weave.py as exact polygons in lid mm, cut to the face rules.

  python3 -I cane_polygons.py     writes build/cane/art/cane.png

Each strand piece is clipped to the field (the face less the 3 mm border, with rounded
corners), then
cut back 1 mm from every hole (a circle polygon with a vertex every 0.2 mm or
less, drawn just outside the 1 mm ring) and 1 mm from the window. A piece the cuts
leave under MIN_WIDTH wide is dropped. No raster, no simplifying.
"""
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np
from PIL import Image, ImageDraw

import candidates_b as b
import weave

CLEAR = b.CLEAR
ARC_STEP = 0.2        # mm between circle vertices at most
NUDGE = (1.7e-7, 0.9e-7)   # keeps cuts off exact coincidence with strand edges, mm
ORIGIN = (2.5 - 2 * weave.PITCH, 5.0 - 13 * weave.PITCH)   # same parity as weave.py proof


# --- Polygon tools (lists of (x, y) tuples, counter-clockwise) -------------------

def signed_area(p):
  return 0.5 * sum(p[i][0] * p[(i + 1) % len(p)][1] - p[(i + 1) % len(p)][0] * p[i][1]
                   for i in range(len(p)))


def hull(points):
  pts = sorted(set(points))
  if len(pts) < 3:
    return pts
  cross = lambda o, a, c: (a[0] - o[0]) * (c[1] - o[1]) - (a[1] - o[1]) * (c[0] - o[0])
  lo, up = [], []
  for p in pts:
    while len(lo) >= 2 and cross(lo[-2], lo[-1], p) <= 0:
      lo.pop()
    lo.append(p)
  for p in reversed(pts):
    while len(up) >= 2 and cross(up[-2], up[-1], p) <= 0:
      up.pop()
    up.append(p)
  return lo[:-1] + up[:-1]


def piece_width(p):
  """weave.width() of the polygon's convex hull; equal to it for a convex piece."""
  h = hull(p)
  return weave.width(h) if len(h) >= 3 else 0.0


def field_polygon():
  """The field as a counter-clockwise convex polygon: straight sides and corner
  arcs of FIELD_CORNER_R, vertices on the arcs every ARC_STEP mm or less."""
  r = b.FIELD_CORNER_R - 1e-4     # a hair inside, so rounding to 0.1 um cannot cross the arc
  n = math.ceil(math.pi / 2 * r / ARC_STEP)
  poly = []
  for k, (cx, cy) in enumerate(((182.0, -142.0), (182.0, -3.0), (3.0, -3.0), (3.0, -142.0))):
    for i in range(n + 1):
      a = math.radians(-90 + 90 * k) + math.pi / 2 * i / n
      poly.append((cx + r * math.cos(a), cy + r * math.sin(a)))
  return poly


def clip_convex(poly, clip):
  """Intersection of convex polygon poly with the convex counter-clockwise polygon clip."""
  for i in range(len(clip)):
    a, d = clip[i], clip[(i + 1) % len(clip)]
    ex, ey = d[0] - a[0], d[1] - a[1]
    ln = math.hypot(ex, ey)
    if ln < 1e-12:
      continue
    n = (ey / ln, -ex / ln)
    poly = weave.clip(poly, n, n[0] * a[0] + n[1] * a[1], True)
    if len(poly) < 3:
      return []
  return poly


def inside_convex(pt, c):
  for i in range(len(c)):
    a, d = c[i], c[(i + 1) % len(c)]
    if (d[0] - a[0]) * (pt[1] - a[1]) - (d[1] - a[1]) * (pt[0] - a[0]) <= 0:
      return False
  return True


def difference(s, c):
  """Parts of polygon s outside the convex polygon c, both counter-clockwise
  (Weiler-Atherton: s forward outside c, c backward inside s)."""
  pts, s_at, c_at, entering = [], {}, {}, {}
  ns, nc = len(s), len(c)
  for i in range(ns):
    a, d = s[i], s[(i + 1) % ns]
    for j in range(nc):
      e, f = c[j], c[(j + 1) % nc]
      ds, dc = (d[0] - a[0], d[1] - a[1]), (f[0] - e[0], f[1] - e[1])
      det = ds[0] * dc[1] - ds[1] * dc[0]
      if abs(det) < 1e-12:
        continue
      t = ((e[0] - a[0]) * dc[1] - (e[1] - a[1]) * dc[0]) / det
      u = ((e[0] - a[0]) * ds[1] - (e[1] - a[1]) * ds[0]) / det
      if 0 < t < 1 and 0 < u < 1:
        k = len(pts)
        pts.append((a[0] + t * ds[0], a[1] + t * ds[1]))
        s_at.setdefault(i, []).append((t, k))
        c_at.setdefault(j, []).append((u, k))
        entering[k] = dc[0] * ds[1] - dc[1] * ds[0] > 0
  if not pts:
    return [] if inside_convex(s[0], c) else [s]

  def ring(poly, at):
    items = []
    for i, p in enumerate(poly):
      items.append((p, None))
      items += [(pts[k], k) for _, k in sorted(at.get(i, []))]
    return items

  s_ring, c_ring = ring(s, s_at), ring(c, c_at)
  s_pos = {k: i for i, (_, k) in enumerate(s_ring) if k is not None}
  c_pos = {k: i for i, (_, k) in enumerate(c_ring) if k is not None}
  out, seen = [], set()
  for e in (k for k in range(len(pts)) if not entering[k]):
    if e in seen:
      continue
    seen.add(e)
    poly, i, on_s = [pts[e]], s_pos[e], True
    for _ in range(len(s_ring) + len(c_ring) + 4):
      if on_s:
        i = (i + 1) % len(s_ring)
        p, k = s_ring[i]
        poly.append(p)
        if k is not None:
          assert entering[k], 'cane clip: s left c twice in a row'
          on_s, i = False, c_pos[k]
      else:
        i = (i - 1) % len(c_ring)
        p, k = c_ring[i]
        if k is None:
          poly.append(p)
          continue
        assert not entering[k], 'cane clip: c walk met an entry'
        if k == e:
          break
        poly.append(p)
        seen.add(k)
        on_s, i = True, s_pos[k]
    else:
      raise RuntimeError('cane clip did not close')
    out.append(poly)
  return out


def circle(cx, cy, r):
  """Polygon that contains the circle, one vertex every ARC_STEP mm or less."""
  n = max(16, math.ceil(2 * math.pi * r / ARC_STEP))
  rr = r / math.cos(math.pi / n)
  return [(cx + rr * math.cos(2 * math.pi * k / n), cy + rr * math.sin(2 * math.pi * k / n))
          for k in range(n)]


def cutters():
  """The shapes the weave stays out of: a ring round every hole and round the window."""
  nx, ny = NUDGE
  out = [circle(x + nx, y + ny, dia / 2 + CLEAR) for x, y, dia in b.HOLES]
  x0, y0, x1, y1 = b.OLED
  x0, y0, x1, y1 = x0 - CLEAR + nx, y0 - CLEAR + ny, x1 + CLEAR + nx, y1 + CLEAR + ny
  out.append([(x0, y0), (x1, y0), (x1, y1), (x0, y1)])
  return out


def bbox(p):
  xs, ys = [q[0] for q in p], [q[1] for q in p]
  return min(xs), min(ys), max(xs), max(ys)


def disjoint(a, c):
  return a[2] < c[0] or c[2] < a[0] or a[3] < c[1] or c[3] < a[1]


def cane_pieces():
  """Cut pieces in lid mm, each counter-clockwise."""
  span = max(b.FACE[2] - b.FACE[0], b.FACE[3] - b.FACE[1]) + 2 * weave.PITCH
  cuts = [(c, bbox(c)) for c in cutters()]
  field = field_polygon()
  out = []
  for raw in weave.pieces(-weave.PITCH, span):
    poly = [(x + ORIGIN[0], y + ORIGIN[1]) for x, y in raw]
    if signed_area(poly) < 0:
      poly.reverse()
    poly = clip_convex(poly, field)
    if len(poly) < 3:
      continue
    parts = [poly]
    for c, cb in cuts:
      nxt = []
      for p in parts:
        nxt += [p] if disjoint(bbox(p), cb) else difference(p, c)
      parts = nxt
    out += [p for p in parts if len(p) >= 3 and piece_width(p) >= weave.MIN_WIDTH]
  return out


def cane_doc():
  polys = cane_pieces()
  rnd = lambda p: [[round(x, 4), round(y, 4)] for x, y in p]
  doc = {'units': 'mm', 'kind': 'polygons', 'face': [float(x) for x in b.FACE],
         'field': list(b.FIELD), 'field_corner_r': b.FIELD_CORNER_R, 'face_holes': [list(h) for h in b.HOLES],
         'window': list(b.OLED), 'simplify_mm': 0.0, 'min_loop_mm2': 0.0,
         'count': len(polys), 'area': round(sum(signed_area(p) for p in polys), 3),
         'polygons': [{'outer': rnd(p), 'holes': []} for p in polys]}
  stats = {'points': sum(len(p) for p in polys),
           'narrowest': min(piece_width(p) for p in polys)}
  return doc, stats


def mask_of(doc):
  """The polygons drawn on the face grid, for the proof."""
  k = b.S * 4
  img = Image.new('L', (b.W * 4, b.H * 4), 0)
  d = ImageDraw.Draw(img)
  for q in doc['polygons']:
    d.polygon([((x - b.FACE[0]) * k, (b.FACE[3] - y) * k) for x, y in q['outer']], fill=255)
  return np.asarray(img.resize((b.W, b.H), Image.BOX)) >= 128


if __name__ == '__main__':
  doc, stats = cane_doc()
  out = b.ROOT / 'build' / 'cane' / 'art'
  out.mkdir(parents=True, exist_ok=True)
  b.face_image(mask_of(doc)).save(out / 'cane.png')
  print(f"pieces {doc['count']}, points {stats['points']}, area {doc['area']:.2f} mm2, "
        f"narrowest {stats['narrowest']:.3f} mm")
