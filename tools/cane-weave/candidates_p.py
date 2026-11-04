#!/usr/bin/env python3
"""Candidate lid-face inlay texture, family P: an all-over rose floral.

One 66 mm square tile of hand-placed motifs repeats in half-drop: each column
of tiles drops half a tile, so the motifs read as a scatter with no grid.
Two colours only. Every motif is a light shape drawn from a distance field.
Where a shape lies over another it clears a 0.7 mm dark ring round itself,
and dark lines 0.7 mm wide cut petals, spirals and veins through it. Same
face grid and report as candidates_b.

  candidates_p.py           P0.png, with light coverage of the field
  candidates_p.py --layout  print clashes between motifs and the largest empty discs
"""

import math
import sys

import numpy as np

import candidates_b as cb
from candidates_b import ALLOWED, H, S, W

TILE = 66.0
N = int(TILE * S)
HALF = N // 2
HALO = 0.7       # dark ring round a shape that lies over another
LINE = 0.7       # dark lines cut through a shape
STEM = 1.0
STALK = 0.9
BERRY = 0.9      # berry radius

TU, TV = np.meshgrid((np.arange(N) + 0.5) / S, (np.arange(N) + 0.5) / S)

# Half-drop lattice: one tile right drops half a tile.

COPIES = [(i * TILE, i * TILE / 2 + j * TILE) for i in (-1, 0, 1) for j in (-2, -1, 0, 1, 2)]

ITEMS = []
_ids = [0]


def new_id():
  _ids[0] += 1
  return _ids[0]


NAMES = {}


def start(kind, u, v):
  """Ids for one spray and its shared light group, named for the layout report."""
  tid = new_id()
  NAMES[tid] = f'{kind} at ({u:.0f}, {v:.0f})'
  return tid, new_id()


def add(layer, group, tid, centre, reach, fn):
  """Queue a shape. Layers paint in order; a shape's dark ring spares light
  of its own group, so a leaf and its stem join without a seam."""
  ITEMS.append((layer, group, tid, centre, reach, fn))


# --- Geometry -----------------------------------------------------------------

class Place:
  """Maps a motif's own frame (x right, y down, mm) onto the tile, mirrored
  left for right when asked, then turned `ang` degrees clockwise."""

  def __init__(self, u, v, ang, mirror=False):
    self.u, self.v, self.ang, self.mirror = u, v, ang, mirror

  def pt(self, x, y):
    x = -x if self.mirror else x
    a = math.radians(self.ang)
    return (self.u + x * math.cos(a) - y * math.sin(a),
            self.v + x * math.sin(a) + y * math.cos(a))

  def a(self, ang):
    return self.ang + (180 - ang if self.mirror else ang)

  def bend(self, b):
    return -b if self.mirror else b


def polar(r, deg):
  return r * math.cos(math.radians(deg)), r * math.sin(math.radians(deg))


def frame(x, y, cx, cy, ang, mirror=False):
  """Coordinates along (s) and across (t) a direction `ang` degrees from +u."""
  a = math.radians(ang)
  c, s = math.cos(a), math.sin(a)
  dx, dy = x - cx, y - cy
  t = -dx * s + dy * c
  return dx * c + dy * s, (-t if mirror else t)


def grad_sdf(f):
  """Signed distance near the zero set of an implicit field f."""
  gy, gx = np.gradient(f, 1 / S)
  return f / np.maximum(np.hypot(gx, gy), 0.3)


def ellipse(x, y, a, b):
  q = np.sqrt((x / a) ** 2 + (y / b) ** 2)
  g = np.sqrt((x / (a * a)) ** 2 + (y / (b * b)) ** 2)
  return (q - 1) * q / np.maximum(g, 1e-9)


def convex(x, y, verts):
  """Signed distance (exact inside, near enough outside) to a convex polygon."""
  n = len(verts)
  area = sum(verts[i][0] * verts[(i + 1) % n][1] - verts[(i + 1) % n][0] * verts[i][1]
             for i in range(n))
  sgn = 1 if area > 0 else -1
  f = np.full(np.shape(x), -np.inf)
  for i in range(n):
    (ax, ay), (bx, by) = verts[i], verts[(i + 1) % n]
    ex, ey = bx - ax, by - ay
    f = np.maximum(f, sgn * ((x - ax) * ey - (y - ay) * ex) / math.hypot(ex, ey))
  return f


def bezier(p0, p1, p2, p3, n=28):
  t = np.linspace(0, 1, n)[:, None]
  p0, p1, p2, p3 = (np.asarray(p, float) for p in (p0, p1, p2, p3))
  return (1 - t) ** 3 * p0 + 3 * (1 - t) ** 2 * t * p1 + 3 * (1 - t) * t * t * p2 + t ** 3 * p3


def poly_dist(x, y, pts):
  d2 = np.full(np.shape(x), np.inf)
  for (ax, ay), (bx, by) in zip(pts[:-1], pts[1:]):
    vx, vy = bx - ax, by - ay
    h = np.clip(((x - ax) * vx + (y - ay) * vy) / (vx * vx + vy * vy), 0, 1)
    d2 = np.minimum(d2, (x - ax - h * vx) ** 2 + (y - ay - h * vy) ** 2)
  return np.sqrt(d2)


def on(pts, frac):
  """Point at a fraction along a sampled curve, and its heading in degrees."""
  k = min(int(frac * (len(pts) - 1)), len(pts) - 2)
  (ax, ay), (bx, by) = pts[k], pts[k + 1]
  f = frac * (len(pts) - 1) - k
  return (ax + f * (bx - ax), ay + f * (by - ay)), math.degrees(math.atan2(by - ay, bx - ax))


def profile(x, a, b):
  """Half-width along a blade, 0 at both ends and 1 at its widest."""
  m = (a / (a + b)) ** a * (b / (a + b)) ** b
  xc = np.clip(x, 0, 1)
  return xc ** a * (1 - xc) ** b / m


def blade(s, t, length, hw, a, b, bend):
  """Implicit field of a pointed blade from s = 0 to s = length, its axis
  bowed sideways by `bend` mm at the middle; also the across coordinate
  measured from the bowed axis."""
  xc = np.clip(s / length, 0, 1)
  tb = t - bend * 4 * xc * (1 - xc)
  w = hw * profile(s / length, a, b)
  return np.maximum(np.abs(tb) - w, np.maximum(-s, s - length)), tb


def reach_of(pts, extra):
  pts = np.asarray(pts)
  lo, hi = pts.min(0), pts.max(0)
  return tuple((lo + hi) / 2), float(np.hypot(*(hi - lo)) / 2 + extra)


# --- Motifs -------------------------------------------------------------------

def leaf(group, tid, base, ang, length, hw, bend=0.0, layer=0):
  """A leaf from its base along `ang`: light, with a dark midrib and up to
  two side veins a side, alternating, that stop 1.15 mm short of the edge."""
  bx, by = base
  tip = (bx + length * math.cos(math.radians(ang)), by + length * math.sin(math.radians(ang)))
  centre, reach = reach_of([base, tip], hw + abs(bend) + HALO + 0.5)
  veins = []
  for side, s0 in ((1, 0.24 * length), (-1, 0.24 * length + 1.6)):
    for si in (s0, s0 + 3.2):
      se = si + 0.85 * hw * float(profile(si / length, 0.6, 1.0))
      te = hw * float(profile(se / length, 0.6, 1.0)) - 1.5
      if si < 0.7 * length and te >= 0.8:
        veins.append(bezier((si, 0), (si + 0.2 * (se - si), side * 0.45 * te),
                            (si + 0.6 * (se - si), side * 0.85 * te), (se, side * te), 10))

  def fn(x, y):
    s, t = frame(x, y, bx, by, ang)
    f, tb = blade(s, t, length, hw, 0.6, 1.0, bend)
    s0, s1 = 0.12 * length, 0.72 * length
    rib = np.where(s < s0, np.hypot(s - s0, tb), np.where(s > s1, np.hypot(s - s1, tb), np.abs(tb)))
    cut = rib < LINE / 2
    for v in veins:
      cut |= poly_dist(s, tb, v) < LINE / 2
    return grad_sdf(f), cut

  add(layer, group, tid, centre, reach, fn)


def sepal(group, tid, base, ang, length, hw, bend):
  bx, by = base
  tip = (bx + length * math.cos(math.radians(ang)), by + length * math.sin(math.radians(ang)))
  centre, reach = reach_of([base, tip], hw + abs(bend) + HALO + 0.5)

  def fn(x, y):
    s, t = frame(x, y, bx, by, ang)
    return grad_sdf(blade(s, t, length, hw, 0.5, 1.2, bend)[0]), None

  add(2, group, tid, centre, reach, fn)


def stem(group, tid, pts, width=STEM, thorns=(), layer=0):
  """A light band along a sampled curve; thorns are (fraction along, side)."""
  tris = []
  for frac, side in thorns:
    (px, py), a = on(pts, frac)
    tu, tv = math.cos(math.radians(a)), math.sin(math.radians(a))
    nu, nv = -side * tv, side * tu
    tris.append([(px - 0.9 * tu, py - 0.9 * tv), (px + 0.9 * tu, py + 0.9 * tv),
                 (px - 0.5 * tu + 1.7 * nu, py - 0.5 * tv + 1.7 * nv)])
  centre, reach = reach_of(pts, width + 2.5 + HALO)

  def fn(x, y):
    d = poly_dist(x, y, pts) - width / 2
    for tri in tris:
      d = np.minimum(d, convex(x, y, tri))
    return d, None

  add(layer, group, tid, centre, reach, fn)


def disc(group, tid, centre, r, layer=0):
  cx, cy = centre
  add(layer, group, tid, centre, r + HALO + 0.5, lambda x, y: (np.hypot(x - cx, y - cy) - r, None))


# Rose petal rings as (count, centre radius, half-width across, half-depth,
# turn in degrees), sizes as fractions of the rose radius; then the core.

LARGE = [(5, 0.52, 0.56, 0.46, 0), (5, 0.36, 0.44, 0.36, 36), (4, 0.22, 0.32, 0.25, 18)]
LARGE_CORE = 0.28
SMALL = [(5, 0.50, 0.60, 0.50, 0)]
SMALL_CORE = 0.5
PITCH = 1.75


def rose(tid, centre, R, ang, mirror, rings, core):
  """A rose seen from above: rings of overlapping petals painted outside in,
  each petal clearing a dark line round itself, round a core with a spiral."""
  cx, cy = centre
  group = new_id()

  def fn(x, y):
    s, t = frame(x, y, cx, cy, ang, mirror)
    lit = np.zeros(s.shape, bool)
    outline = np.full(s.shape, np.inf)
    for k, (n, rho, a, b, turn) in enumerate(rings):
      for j in range(n):
        c, sn = math.cos(math.radians(turn + 360 * j / n)), math.sin(math.radians(turn + 360 * j / n))
        d = ellipse(-s * sn + t * c, s * c + t * sn - rho * R, a * R, b * R)
        if k == 0:
          outline = np.minimum(outline, d)
        lit[d < LINE] = False
        lit[d < 0] = True
    r = np.hypot(s, t)
    rc = core * R
    lit[r < rc + LINE] = False
    lit[r < rc] = True
    ph = (r - 0.5) / PITCH - np.arctan2(t, s) / (2 * math.pi)
    lit &= ~((np.abs(ph - np.round(ph)) * PITCH < LINE / 2) & (r > 0.5) & (r < rc))
    return outline, (outline < 0) & ~lit

  add(4, group, tid, centre, R + HALO + 1, fn)


def bud(tid, spray, p, base, ang, length):
  """A bud in side view: a light teardrop with one dark wrap line, two short
  sepals splayed over its foot and a round hip below, both part of the spray."""
  bx, by = p.pt(*base)
  a = p.a(ang)
  hw = 0.40 * length
  wl = lambda f: hw * float(profile(f, 0.55, 1.3))
  wrap = bezier((0.2 * length, -0.98 * wl(0.2)), (0.38 * length, -0.3 * hw),
                (0.5 * length, 0.15 * hw), (0.68 * length, 0.12 * hw), 16)
  tip = (bx + length * math.cos(math.radians(a)), by + length * math.sin(math.radians(a)))
  centre, reach = reach_of([(bx, by), tip], hw + HALO + 0.5)
  group = new_id()

  def body(x, y):
    s, t = frame(x, y, bx, by, a, p.mirror)
    f, _ = blade(s, t, length, hw, 0.55, 1.3, 0)
    return grad_sdf(f), poly_dist(s, t, wrap) < LINE / 2

  add(1, group, tid, centre, reach, body)
  for side in (1, -1):
    ox, oy = polar(side * 0.45, ang + 90)
    sepal(spray, tid, p.pt(base[0] + ox, base[1] + oy), p.a(ang + side * 38), 0.4 * length, 0.8,
          p.bend(side * 0.3))
  hx, hy = polar(-0.5, ang)
  disc(spray, tid, p.pt(base[0] + hx, base[1] + hy), 1.05, layer=2)


def flower(tid, centre, R, ang):
  """Five round petals parted by dark slits, a dark ring and a light eye."""
  cx, cy = centre
  group = new_id()

  def fn(x, y):
    s, t = frame(x, y, cx, cy, ang)
    r = np.hypot(s, t)
    d = r - 0.45 * R
    cut = np.abs(r - 1.2) < LINE / 2
    for k in range(5):
      c, sn = math.cos(math.radians(72 * k)), math.sin(math.radians(72 * k))
      d = np.minimum(d, np.hypot(s - 0.58 * R * c, t - 0.58 * R * sn) - 0.42 * R)
      cb_, sb_ = math.cos(math.radians(72 * k + 36)), math.sin(math.radians(72 * k + 36))
      cut |= (np.abs(-s * sb_ + t * cb_) < LINE / 2) & (s * cb_ + t * sb_ > 1.2)
    return d, cut

  add(3, group, tid, centre, R + HALO + 0.5, fn)


# --- Sprays: motifs in their own frame, y down ---------------------------------

def large_spray(p, R):
  """A large rose with two leaves behind its top and a thorned stem below
  carrying a leaf and a bud."""
  tid, g = start('large rose', p.u, p.v)
  leaf(g, tid, p.pt(*polar(0.45 * R, 205)), p.a(200), 15, 3.0, p.bend(0.8))
  leaf(g, tid, p.pt(*polar(0.45 * R, 335)), p.a(338), 14, 2.9, p.bend(-0.8))
  pts = bezier(p.pt(1.5, 0.6 * R), p.pt(4.5, R + 1), p.pt(8, R + 5), p.pt(6.5, R + 10))
  stem(g, tid, pts, thorns=[(0.62, -1 if p.mirror else 1)])
  pt, a = on(pts, 0.5)
  leaf(g, tid, pt, a + p.bend(55), 10, 2.4, p.bend(-0.5))
  bud(tid, g, p, (6.5, R + 10), 107, 9.0)
  rose(tid, p.pt(0, 0), R, p.ang + 17, p.mirror, LARGE, LARGE_CORE)


def small_spray(p, R):
  """A small rose on a stem with a pair of leaves, one leaf behind it."""
  tid, g = start('small rose', p.u, p.v)
  pts = bezier(p.pt(0, 0.5 * R), p.pt(1.5, R + 3), p.pt(-1.5, R + 7), p.pt(0.5, R + 11))
  stem(g, tid, pts)
  pt, a = on(pts, 0.55)
  leaf(g, tid, pt, a + 52, 9.5, 2.3, 0.4)
  leaf(g, tid, pt, a - 52, 9.0, 2.2, -0.4)
  leaf(g, tid, p.pt(*polar(0.4 * R, 300)), p.a(295), 10, 2.4, p.bend(0.5))
  rose(tid, p.pt(0, 0), R, p.ang + 40, p.mirror, SMALL, SMALL_CORE)


def bud_spray(p):
  """A thorned stem with a bud at its top and two leaves."""
  tid, g = start('bud', p.u, p.v)
  pts = bezier(p.pt(0, 9), p.pt(1.2, 4), p.pt(-1.0, 0), p.pt(0, -3))
  stem(g, tid, pts, thorns=[(0.3, 1), (0.66, -1)])
  pt, a = on(pts, 0.25)
  leaf(g, tid, pt, a - 50, 9.5, 2.3, 0.4)
  pt, a = on(pts, 0.55)
  leaf(g, tid, pt, a + 48, 8.5, 2.2, -0.4)
  bud(tid, g, p, (0, -3), -90, 8.5)


def berry_spray(p):
  """Berries on thin stalks off one stem, with a leaf."""
  tid, g = start('berries', p.u, p.v)
  stem(g, tid, bezier(p.pt(0, 9), p.pt(0.8, 5), p.pt(-0.6, 1), p.pt(0, -6.9)))
  for end, fork, mid in (((-3.0, -2.4), (0, 0.6), (-1.8, -0.4)),
                         ((3.0, -4.4), (0, -1.6), (1.8, -2.6)),
                         ((-2.7, -6.7), (-0.1, -3.9), (-1.8, -4.8))):
    stem(g, tid, bezier(p.pt(*fork), p.pt(*mid), p.pt(*mid), p.pt(*end), 12), STALK)
    disc(g, tid, p.pt(*end), BERRY)
  disc(g, tid, p.pt(0, -7.8), BERRY)
  leaf(g, tid, p.pt(0.4, 5), p.a(-28), 8.5, 2.3, p.bend(-0.4))


def flower_spray(p, R=3.6, leaves=True):
  """A five-petal flower, with two leaves behind it when asked."""
  tid, g = start('flower', p.u, p.v)
  if leaves:
    leaf(g, tid, p.pt(*polar(1.2, 125)), p.a(125), 8.5, 2.3, p.bend(0.4))
    leaf(g, tid, p.pt(*polar(1.2, 240)), p.a(240), 8.0, 2.2, p.bend(-0.4))
  flower(tid, p.pt(0, 0), R, p.ang)


def lone_leaf(p, length):
  """A single leaf centred on the place, pointing along its turn."""
  tid, g = start('leaf', p.u, p.v)
  bx, by = polar(-length / 2, p.ang)
  leaf(g, tid, (p.u + bx, p.v + by), p.ang, length, 0.24 * length, p.bend(0.4))


# One tile, 66 mm square, u right and v down: each spray with its place
# (u, v, turn in degrees, mirrored) and size.

LAYOUT = [
  (large_spray, (17, 18, -80), 12.0),
  (large_spray, (50, 63, 100, True), 11.0),
  (small_spray, (40, 31, 15), 5.8),
  (small_spray, (15, 48, 45, True), 6.0),
  (small_spray, (62, 30, 10, True), 5.5),
  (bud_spray, (65, 11, 25)),
  (bud_spray, (21, 64, -60, True)),
  (berry_spray, (32, 4, 220)),
  (flower_spray, (27, 34, 260)),
  (flower_spray, (68, 58, 0)),
  (flower_spray, (49, 37, 240)),
  (flower_spray, (4.8, 49.2, 30), 3.4, False),
  (lone_leaf, (4.5, 4.5, 35), 11),
  (lone_leaf, (47.2, 47.6, 150, True), 9.5),
  (lone_leaf, (52.0, 28.4, 260), 9),
  (lone_leaf, (30.6, 26.2, 115, True), 9),
  (lone_leaf, (39.5, 7.8, 200), 9),
  (lone_leaf, (10.2, 38.5, 320, True), 9),
]


def compose():
  ITEMS.clear()
  NAMES.clear()
  for fn, place, *size in LAYOUT:
    fn(Place(*place), *size)


# --- Painting -------------------------------------------------------------------

def paint(report=False):
  light = np.zeros((N, N), bool)
  owner = np.full((N, N), -1, np.int32)
  who = np.full((N, N), -1, np.int32)
  near = np.zeros((N, N), bool)
  clash = {}
  for layer, group, tid, (cu, cv), reach, fn in sorted(ITEMS, key=lambda it: it[0]):
    for ou, ov in COPIES:
      i0, i1 = max(0, int((cu + ou - reach) * S)), min(N, int(math.ceil((cu + ou + reach) * S)))
      j0, j1 = max(0, int((cv + ov - reach) * S)), min(N, int(math.ceil((cv + ov + reach) * S)))
      if i1 - i0 < 3 or j1 - j0 < 3:
        continue
      sdf, cut = fn(TU[j0:j1, i0:i1] - ou, TV[j0:j1, i0:i1] - ov)
      lw, ow, ww = light[j0:j1, i0:i1], owner[j0:j1, i0:i1], who[j0:j1, i0:i1]
      halo = (sdf < HALO) & lw & (ow != group)
      if report:
        hit = (sdf < HALO) & lw & (ww != tid) & (ww >= 0)
        for other, n in zip(*np.unique(ww[hit], return_counts=True)):
          key = (min(tid, other), max(tid, other))
          clash[key] = clash.get(key, 0) + int(n)
        near[j0:j1, i0:i1] |= sdf < HALO + 1.0
      lw[halo] = False
      ow[halo] = -1
      body = sdf < 0
      lw[body] = True
      ow[body] = group
      ww[body] = tid
      if cut is not None:
        c = cut & body
        lw[c] = False
        ow[c] = -1
  return light, near, clash


def tiled(tile, rows, cols):
  """Tile values at pixel rows and columns of the half-drop repeat."""
  i = cols // N
  return tile[(rows[:, None] - i[None, :] * HALF) % N, (cols % N)[None, :]]


def layout_report():
  from scipy.ndimage import distance_transform_edt
  light, near, clash = paint(report=True)
  print(f'tile light {100 * light.mean():.1f} %')
  for (a, b), n in sorted(clash.items(), key=lambda kv: -kv[1]):
    if n > 20:
      print(f'  clash {NAMES.get(a, a)} and {NAMES.get(b, b)}: {n / S / S:.1f} mm2')
  span = np.arange(-N, 2 * N)
  d = distance_transform_edt(~tiled(near, span, span))[N:2 * N, N:2 * N] / S
  for _ in range(16):
    j, i = np.unravel_index(np.argmax(d), d.shape)
    r = d[j, i]
    if r < 2.5:
      break
    u, v = (i + 0.5) / S, (j + 0.5) / S
    print(f'  hole at ({u:.1f}, {v:.1f}) radius {r:.1f}')
    d[np.hypot(TU - u, TV - v) < r + 3] = 0


FILLET = cb.disc(9)
SPECK = cb.disc(7)


def finish(light):
  """Inside ALLOWED, round convex corners to 0.45 mm and fill dark specks
  under 0.7 mm, so petal junctions and tips meet the print limits as drawn;
  print the share of light pixels this changed."""
  start = light & ALLOWED
  done = cb.closing(cb.opening(start, FILLET), SPECK) & ALLOWED
  print(f'P0: corner rounding changed {100 * np.count_nonzero(done != start) / np.count_nonzero(start):.2f} %'
        ' of light pixels')
  return done


def p0_roses():
  light, _, _ = paint()
  return finish(tiled(light, np.arange(H), np.arange(W)))


def main(args):
  compose()
  if '--layout' in args:
    layout_report()
    return
  cb.OUT.mkdir(parents=True, exist_ok=True)
  img, _ = cb.render('P0', p0_roses)
  lit = np.all(np.asarray(img) == cb.LIGHT, axis=2)
  print(f'P0: light covers {100 * np.count_nonzero(lit & ALLOWED) / np.count_nonzero(ALLOWED):.1f} % of the field')


if __name__ == '__main__':
  main(sys.argv[1:])
