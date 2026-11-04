#!/usr/bin/env python3
"""Export a lid inlay pattern in lid coordinates, mm, to build/cane/<name>-inlay.json.

  inlay_export.py C12     circles from candidates_c.c12_circle_list
  inlay_export.py G5      circles from candidates_g.g5_circle_list
  inlay_export.py N5a     polygons traced from candidates_n.n5a_triaxial
  inlay_export.py <stem>  polygons traced from build/cane/art/<stem>.npy (art_inlay.py)

Lid coordinates: x = u + FACE[0], y = FACE[3] - v. Circles are [x, y, r]. Polygons
are outer loops counter-clockwise with their holes clockwise; each loop is open
(the last point does not repeat the first).
"""

import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np

from candidates_b import ALLOWED, ALLOWED_TO_EDGES, FACE, FIELD, HOLES, OLED, ROOT, S

OUT_DIR = ROOT / 'build' / 'cane'

SIMPLIFY_MM = 0.03
MIN_LOOP_MM2 = 0.5


# --- Circles ------------------------------------------------------------------

def circle_source(name):
  if name == 'c12':
    from candidates_c import c12_circle_list
    return c12_circle_list()
  if name == 'g5':
    from candidates_g import g5_circle_list
    return g5_circle_list()
  return None


def circles_doc(uvr):
  circles = [[round(float(u + FACE[0]), 4), round(float(FACE[3] - v), 4), round(float(r), 4)]
             for u, v, r in uvr]
  return {'units': 'mm', 'kind': 'circles', 'face': [float(x) for x in FACE],
          'count': len(circles), 'circles': circles}


def circle_report(doc):
  c = np.array(doc['circles'])
  print(f"count {doc['count']}")
  print(f'r min {c[:, 2].min()} max {c[:, 2].max()}')
  print(f'x min {c[:, 0].min()} max {c[:, 0].max()}')
  print(f'y min {c[:, 1].min()} max {c[:, 1].max()}')

  # Fusion needs one profile per circle, so no two circles may touch.

  from scipy.spatial import cKDTree
  d, i = cKDTree(c[:, :2]).query(c[:, :2], k=2)
  gap = d[:, 1] - c[:, 2] - c[i[:, 1], 2]
  print(f'smallest gap between circles {gap.min():.3f} mm')
  if gap.min() <= 0:
    raise SystemExit('circles touch')
  print(f'dot area {sum(math.pi * r * r for r in c[:, 2]):.2f} mm2')


# --- Polygons -----------------------------------------------------------------

# Marching squares on the mask at level 0.5, light kept on the left of each
# segment in image coordinates (x right, y down). Edge midpoints of a cell
# whose top-left pixel centre is (i, j): T top, R right, B bottom, L left.
# Diagonal light pixels stay apart (a saddle cell cuts off both light corners).

_MID = {'T': (0.5, 0.0), 'R': (1.0, 0.5), 'B': (0.5, 1.0), 'L': (0.0, 0.5)}
_SEGMENTS = {
  1: [('B', 'L')], 2: [('R', 'B')], 3: [('R', 'L')], 4: [('T', 'R')],
  5: [('T', 'R'), ('B', 'L')], 6: [('T', 'B')], 7: [('T', 'L')], 8: [('L', 'T')],
  9: [('B', 'T')], 10: [('L', 'T'), ('R', 'B')], 11: [('R', 'T')], 12: [('L', 'R')],
  13: [('B', 'R')], 14: [('L', 'B')],
}


def trace(mask):
  """Closed loops round the light pixels of mask, in pixel-centre coordinates
  (x = column, y = row), points at half-pixel steps. Returns (loops, saddles)."""
  p = np.pad(mask, 1).astype(np.uint8)
  case = p[:-1, :-1] * 8 + p[:-1, 1:] * 4 + p[1:, 1:] * 2 + p[1:, :-1]
  starts, ends = [], []
  for c, segs in _SEGMENTS.items():
    j, i = np.nonzero(case == c)
    for a, b in segs:
      starts.append(np.stack([2 * i + 2 * _MID[a][0], 2 * j + 2 * _MID[a][1]], 1))
      ends.append(np.stack([2 * i + 2 * _MID[b][0], 2 * j + 2 * _MID[b][1]], 1))
  saddles = int(np.count_nonzero((case == 5) | (case == 10)))

  # Each midpoint starts one segment and ends one, so the next segment is the
  # one that starts where this one ends. Points are doubled to integers.

  st = np.concatenate(starts).astype(np.int64)
  en = np.concatenate(ends).astype(np.int64)
  width = int(st[:, 0].max()) + 3
  ks, ke = st[:, 1] * width + st[:, 0], en[:, 1] * width + en[:, 0]
  order = np.argsort(ks)
  pos = np.searchsorted(ks[order], ke)
  nxt = order[pos]
  if not np.array_equal(ks[nxt], ke):
    raise RuntimeError('open contour')

  seen = np.zeros(len(st), bool)
  loops = []
  for s in range(len(st)):
    if seen[s]:
      continue
    idx, k = [], s
    while not seen[k]:
      seen[k] = True
      idx.append(k)
      k = nxt[k]
    loops.append(st[idx] / 2.0 - 1.0)
  return loops, saddles


def signed_area(pts):
  x, y = pts[:, 0], pts[:, 1]
  return 0.5 * float(np.dot(x, np.roll(y, -1)) - np.dot(np.roll(x, -1), y))


def dp_open(pts, tol):
  """Douglas-Peucker on an open polyline; keeps both ends."""
  keep = np.zeros(len(pts), bool)
  keep[0] = keep[-1] = True
  stack = [(0, len(pts) - 1)]
  while stack:
    a, b = stack.pop()
    if b <= a + 1:
      continue
    seg = pts[b] - pts[a]
    rel = pts[a + 1:b] - pts[a]
    n = math.hypot(*seg)
    if n == 0:
      d = np.hypot(rel[:, 0], rel[:, 1])
    else:
      d = np.abs(seg[0] * rel[:, 1] - seg[1] * rel[:, 0]) / n
    k = int(np.argmax(d))
    if d[k] > tol:
      m = a + 1 + k
      keep[m] = True
      stack += [(a, m), (m, b)]
  return pts[keep]


def dp_closed(pts, tol):
  """Douglas-Peucker on a closed loop, split at the point farthest from the first."""
  far = int(np.argmax(np.hypot(*(pts - pts[0]).T)))
  ring = np.vstack([pts, pts[:1]])
  first = dp_open(ring[:far + 1], tol)
  second = dp_open(ring[far:], tol)
  return np.vstack([first[:-1], second[:-1]])


def inside(pt, poly):
  """Even-odd test of point pt against the closed polygon poly."""
  x, y = pt
  xs, ys = poly[:, 0], poly[:, 1]
  xn, yn = np.roll(xs, -1), np.roll(ys, -1)
  cross = (ys > y) != (yn > y)
  with np.errstate(divide='ignore', invalid='ignore'):
    xi = xs + (y - ys) * (xn - xs) / (yn - ys)
  return bool(np.count_nonzero(cross & (x < xi)) % 2)


def polygons_doc(mask, simplify=SIMPLIFY_MM, min_loop=MIN_LOOP_MM2):
  loops, saddles = trace(mask)

  # Pixel centres to lid mm: u = (col + 0.5) / S, x = u + FACE[0]; y flips.

  lid = []
  for pts in loops:
    mm = np.column_stack([(pts[:, 0] + 0.5) / S + FACE[0], FACE[3] - (pts[:, 1] + 0.5) / S])
    mm = dp_closed(mm, simplify)
    a = signed_area(mm)
    if abs(a) >= min_loop and len(mm) >= 3:
      lid.append((mm, a))

  # Flipping y turns the light-on-the-left rule into: outer loops have
  # positive area, holes negative.

  outers = [(mm, a) for mm, a in lid if a > 0]
  holes = [(mm, a) for mm, a in lid if a < 0]
  boxes = [(mm[:, 0].min(), mm[:, 1].min(), mm[:, 0].max(), mm[:, 1].max()) for mm, _ in outers]
  owned = [[] for _ in outers]
  for hm, ha in holes:
    pt = hm[0]
    best = None
    for k, (om, oa) in enumerate(outers):
      x0, y0, x1, y1 = boxes[k]
      if x0 <= pt[0] <= x1 and y0 <= pt[1] <= y1 and inside(pt, om):
        if best is None or oa < outers[best][1]:
          best = k
    if best is None:
      raise RuntimeError('hole with no outer loop')
    owned[best].append(hm)

  rnd = lambda mm: [[round(float(x), 4), round(float(y), 4)] for x, y in mm]
  polys = [{'outer': rnd(om), 'holes': [rnd(h) for h in owned[k]]}
           for k, (om, _) in enumerate(outers)]
  area = sum(a for _, a in outers) + sum(a for _, a in holes)
  doc = {'units': 'mm', 'kind': 'polygons', 'face': [float(x) for x in FACE],
         'field': list(FIELD), 'face_holes': [list(h) for h in HOLES], 'window': list(OLED),
         'simplify_mm': simplify, 'min_loop_mm2': min_loop,
         'count': len(polys), 'area': round(area, 3), 'polygons': polys}
  stats = {'loops': len(loops), 'saddles': saddles, 'holes': len(holes),
           'points': sum(len(mm) for mm, _ in lid)}
  return doc, stats


def polygon_source(name):
  if name == 'n5a':
    from candidates_n import n5a_triaxial
    return n5a_triaxial() & ALLOWED
  art = OUT_DIR / 'art' / f'{name}.npy'
  if art.exists():
    return np.load(art) & ALLOWED_TO_EDGES
  return None


def polygon_report(doc, stats, mask):
  light = np.count_nonzero(mask) / (S * S)
  print(f"pieces {doc['count']}, holes {stats['holes']}, loops traced {stats['loops']}, "
        f"saddle cells {stats['saddles']}, points after simplifying {stats['points']}")
  print(f"polygon area {doc['area']:.2f} mm2, mask light area {light:.2f} mm2, "
        f"difference {100 * (doc['area'] - light) / light:+.3f} %")
  if abs(doc['area'] - light) > 0.01 * light:
    raise SystemExit('polygon area off the mask by more than 1 %')


# --- Main ---------------------------------------------------------------------

def export(name, out=None, simplify=SIMPLIFY_MM, min_loop=MIN_LOOP_MM2):
  key = name.lower()
  out = Path(out) if out else OUT_DIR / f'{key}-inlay.json'
  uvr = circle_source(key)
  if uvr is not None:
    doc = circles_doc(uvr)
    circle_report(doc)
  else:
    mask = polygon_source(key)
    if mask is None:
      raise SystemExit(f'no inlay pattern named {name}')
    doc, stats = polygons_doc(mask, simplify, min_loop)
    polygon_report(doc, stats, mask)
  out.parent.mkdir(parents=True, exist_ok=True)
  out.write_text(json.dumps(doc) + '\n')
  print(f'wrote {out}')
  return doc


if __name__ == '__main__':
  args, opts, i = [], {}, 1
  while i < len(sys.argv):
    if sys.argv[i] in ('--simplify', '--min-loop') and i + 1 < len(sys.argv):
      opts['simplify' if sys.argv[i] == '--simplify' else 'min_loop'] = float(sys.argv[i + 1])
      i += 2
    else:
      args.append(sys.argv[i])
      i += 1
  if len(args) not in (1, 2):
    raise SystemExit(__doc__)
  export(*args, **opts)
