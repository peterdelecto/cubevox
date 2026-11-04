#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family Q: dark botanical.

An all-over scatter of small open flowers, side-view blooms, buds, long
leaves and pinnate sprigs, drawn as flat light shapes with dark lines cut
through them. One 42 mm tile of hand-placed motifs repeats in half-drop: each
column of tiles sits 21 mm below the column to its left. Each motif is drawn
once on its own patch and stamped at every repeat. Motifs paint back to
front, and each shape clears a dark halo round itself in whatever lies
beneath, so overlaps read as one shape tucked behind another. Petals are
split by single dark lines; each flower centre is one dark ring round a light
disc. The window and hole clearances cut motifs like fabric round a button;
light pieces under 5 mm2 left by those cuts are dropped. Same face grid and
report as candidates_b.

  candidates_q.py           every candidate as <name>.png
  candidates_q.py Q0        only the named candidates
  candidates_q.py --map     which motif covers each 1 mm cell of one tile
"""

import math
import sys

import numpy as np
from scipy import ndimage
from scipy.spatial import cKDTree

import candidates_b as cb
from candidates_b import ALLOWED, H, LIGHT, S, W, closing, disc

TILE = 42.0              # mm, the repeat
DROP = 21.0              # each tile column sits this far below the one to its left
ORIGIN = (0.0, 0.0)      # face u, v of one tile's top-left corner

# Dark marks print at 0.6 mm or wider. A 0.65 mm line and a 0.7 mm halo
# (0.60 to 0.67 mm of dark pixels) survive the print pass at every angle.

GAP = 0.7                # dark halo round a shape painted over another, mm
LINE = 0.65              # dark line cut through a shape, mm
RING = 0.7               # dark ring round each flower's light centre disc, mm
STEM = 1.0               # light stem width, mm
SPRIG_STEM = 0.9         # light stem width of the pinnate sprigs, mm
ROUND = 0.45             # tips and corners of petals and leaves, radius mm
FILLET = disc(8)         # closes dark corners under 0.8 mm where sprig leaflets join the stem


# --- Raster tools -------------------------------------------------------------

def grow(m, gap):
  """Pixels within `gap` mm of m."""
  if not m.any():
    return m.copy()
  return ndimage.distance_transform_edt(~m) < gap * S


def soften(m, r=ROUND):
  """m with tips and convex corners rounded to r mm: an opening by a disc."""
  if not m.any():
    return m
  core = ndimage.distance_transform_edt(m) > r * S
  return grow(core, r) & m


class Layers:
  """Light shapes painted back to front on one motif's patch."""

  def __init__(self, shape):
    self.shape = np.zeros(shape, bool)      # silhouette
    self.light = np.zeros(shape, bool)

  def over(self, m, light=None):
    """Paint m on top, clearing a GAP halo round it in what lies beneath."""
    self.light &= ~grow(m, GAP)
    self.light |= m if light is None else light
    self.shape |= m

  def join(self, m, light=None):
    """Paint m merged with whatever light it touches."""
    self.light |= m if light is None else light
    self.shape |= m


def turn(x, y, ox, oy, angle):
  """x, y in the frame placed at (ox, oy) and turned `angle` degrees (v down)."""
  a = math.radians(angle)
  dx, dy = x - ox, y - oy
  return dx * math.cos(a) + dy * math.sin(a), -dx * math.sin(a) + dy * math.cos(a)


def polar(r, angle):
  a = math.radians(angle)
  return r * math.cos(a), r * math.sin(a)


# --- Curves -------------------------------------------------------------------

def bezier(ctrl, step=0.03):
  """Points about `step` mm apart along the Bezier curve on ctrl, and the arc
  length from the start to each."""
  ctrl = np.asarray(ctrl, float)
  n = len(ctrl) - 1
  rough = np.sum(np.hypot(*np.diff(ctrl, axis=0).T))
  t = np.linspace(0, 1, max(8, int(rough / step) + 1))[:, None]
  pts = sum(math.comb(n, k) * t ** k * (1 - t) ** (n - k) * ctrl[k] for k in range(n + 1))
  s = np.concatenate([[0.0], np.cumsum(np.hypot(*np.diff(pts, axis=0).T))])
  return pts, s


def near(x, y, pts, reach):
  """Distance from each pixel to the polyline pts and the index of the
  nearest point; inf and len(pts) beyond `reach` mm."""
  lo, hi = pts.min(0) - reach, pts.max(0) + reach
  sel = (x > lo[0]) & (x < hi[0]) & (y > lo[1]) & (y < hi[1])
  d = np.full(x.shape, np.inf)
  i = np.full(x.shape, len(pts))
  d[sel], i[sel] = cKDTree(pts).query(np.column_stack([x[sel], y[sel]]),
                                      distance_upper_bound=reach)
  return d, i


def point_on(ctrl, f):
  """Point and heading (degrees) at arc-length fraction f along the curve."""
  pts, s = bezier(ctrl)
  i = int(np.clip(np.searchsorted(s, f * s[-1]), 1, len(pts) - 2))
  dx, dy = pts[i + 1] - pts[i - 1]
  return pts[i], math.degrees(math.atan2(dy, dx))


# --- Shapes -------------------------------------------------------------------

def band(x, y, ctrl, width=STEM):
  """A light stem of `width` mm along the curve on ctrl, round ends."""
  pts, _ = bezier(ctrl)
  d, _ = near(x, y, pts, width)
  return d < width / 2


def ellipse(x, y, cx, cy, a, b, angle=0.0, point=0.0):
  """Ellipse a mm across and b mm along its axis; the axis points to
  `angle` + 90 degrees. `point` narrows the end toward angle - 90."""
  X, Y = turn(x, y, cx, cy, angle)
  q = Y / b
  across = a * (1 - point * np.clip(-q, 0, 1))
  return (X / across) ** 2 + q * q < 1


def leaf(x, y, base, angle, length, bend=0.0, width=0.32):
  """Lanceolate leaf from base toward `angle` degrees, its spine bowed
  sideways by bend * length. Returns the blade and its dark midrib, drawn
  only where both halves beside it stay 0.95 mm wide."""
  a = math.radians(angle)
  ux, uy = math.cos(a), math.sin(a)
  bx, by = base
  ctrl = [(bx, by),
          (bx + 0.5 * length * ux - bend * length * uy, by + 0.5 * length * uy + bend * length * ux),
          (bx + length * ux, by + length * uy)]
  pts, s = bezier(ctrl)
  half = 0.5 * width * length * np.sin(np.pi * (s / s[-1]) ** 0.72)
  d, i = near(x, y, pts, half.max())
  blade = soften(d < np.append(half, 0.0)[i])
  wide = pts[half > LINE / 2 + 0.95]
  rib = near(x, y, wide, LINE)[0] < LINE / 2 if len(wide) > 1 else np.zeros_like(blade)
  return blade, rib


def add_leaf(L, x, y, *args, **kw):
  """Paint a leaf over what is there."""
  blade, rib = leaf(x, y, *args, **kw)
  L.over(blade, blade & ~rib)


def join_filleted(L, parts, dark):
  """Paint stems and leaves merged as one shape, the dark corners where they
  meet filleted to 0.8 mm, then cut the dark lines (midribs) through it."""
  m = closing(np.logical_or.reduce(parts), FILLET)
  L.join(m, m & ~np.logical_or.reduce(dark))


def capsule(x, y, p0, p1, width):
  """Dark or light stroke `width` mm wide from p0 to p1, round ends."""
  (x0, y0), (x1, y1) = p0, p1
  dx, dy = x1 - x0, y1 - y0
  t = np.clip(((x - x0) * dx + (y - y0) * dy) / (dx * dx + dy * dy), 0, 1)
  return np.hypot(x - x0 - t * dx, y - y0 - t * dy) < width / 2


def lobes(x, y, n, tip, valley, phase=0.0, point=None):
  """Disc whose rim swells into n lobes, out to `tip` on each lobe axis and
  in to `valley` between: round lobes, or pointed ones with sides curving
  in by the power `point`. Returns the shape, each pixel's distance to the
  nearest boundary ray between lobes and to the nearest lobe axis, and r."""
  r = np.hypot(x, y)
  step = 360 / n
  a = (np.degrees(np.arctan2(y, x)) - phase) / step
  f = np.abs(a - np.round(a))                    # 0 on a lobe axis, 0.5 between lobes
  z = 2 * f
  if point is None:
    rim = valley + (tip - valley) * np.sqrt(np.clip(1 - z * z, 0, 1))
  else:
    rim = valley + (tip - valley) * (1 - z) ** point
  to_divide = r * np.sin(np.radians((0.5 - f) * step))
  to_axis = r * np.sin(np.radians(f * step))
  return soften(r < rim), to_divide, to_axis, r


def centre(L, x, y, disc_r):
  """One dark ring RING wide round a light centre disc of radius disc_r."""
  r = np.hypot(x, y)
  ring = r < disc_r + RING
  L.light &= ~ring
  L.shape |= ring
  L.light |= r < disc_r


# --- Flowers and blooms -------------------------------------------------------

def anemone(L, x, y, R):
  """Six round petals split by dark lines, a short dark vein on each."""
  shape, to_divide, _, r = lobes(x, y, 6, R, 0.8 * R)
  dark = to_divide < LINE / 2
  for k in range(6):
    dark |= capsule(x, y, polar(0.42 * R, 60 * k), polar(0.66 * R, 60 * k), LINE)
  L.over(shape | (r < R), soften(shape & ~dark))
  centre(L, x, y, 1.05)


def wild_rose(L, x, y, R):
  """Five heart-shaped petals: round lobes with a round bite from each tip,
  split by dark lines."""
  shape, to_divide, _, r = lobes(x, y, 5, R, 0.84 * R)
  for k in range(5):
    bx, by = polar(R, 72 * k)
    shape &= ~(np.hypot(x - bx, y - by) < 0.6)
  shape = soften(shape)
  L.over(shape | (r < R), soften(shape & ~(to_divide < LINE / 2)))
  centre(L, x, y, 0.9)


def dahlia(L, x, y, R):
  """Ten pointed petals, and over them a rosette of ten shorter pointed
  petals turned half a step, split by dark lines beyond its solid inner band.
  The rosette's shallow valleys hide where the outer petals narrow between
  its points."""
  outer, _, _, r = lobes(x, y, 10, R, 0.45 * R, point=1.3)
  L.over(outer | (r < 0.85 * R), outer)
  inner, to_divide, _, r = lobes(x, y, 10, 0.6 * R, 0.5 * R, phase=18, point=1.5)
  divides = (to_divide < LINE / 2) & (r > 3.1)
  for k in range(10):
    bx, by = polar(3.1, 36 * k)
    divides |= np.hypot(x - bx, y - by) < LINE / 2
  L.over(inner, soften(inner & ~divides))
  centre(L, x, y, 1.1)


def tulip(L, x, y, h):
  """Side view, opening toward -y, base at the origin, about h mm tall:
  two back petals and a front petal. The back petals stop where their
  strip beside the front petal would narrow under 0.9 mm."""
  k = h / 6.5
  for side in (-1, 1):
    back = ellipse(x, y, side * 1.8 * k, -3.4 * k, 1.15 * k, 2.9 * k, angle=side * 14, point=0.35)
    L.over(soften(back & (y < -2.2 * k)))
  L.over(soften(ellipse(x, y, 0, -3.2 * k, 1.25 * k, 3.2 * k, point=0.3)))


def bud(L, x, y, h):
  """Closed bud, tip toward -y, base at the origin: a teardrop with a short
  dark seam, a round calyx laid over its base."""
  t = np.clip(-y / h, 0, 1)
  w = 0.34 * h * np.sin(np.pi * t ** 0.6)
  body = soften((np.abs(x) < w) & (y < 0))
  seam = (np.abs(x - 0.25 * np.sin(np.pi * t)) < LINE / 2) & (t > 0.22) & (t < 0.55)
  L.over(body, body & ~seam)
  L.over(soften(ellipse(x, y, 0, 0.3, 1.2, 0.8)))


def sprig(L, x, y, ctrl, first=1.2, step=1.4, tilt=50):
  """Pinnate sprig: a stem with 1.2 x 1.6 mm oval leaflets alternating sides
  every `step` mm, each `tilt` degrees off the stem, and one at the tip.
  The joins are filleted so no dark corner is narrower than 0.8 mm."""
  pts, s = bezier(ctrl)
  m = band(x, y, ctrl, SPRIG_STEM)
  pos, side = first, 1
  while pos < s[-1] - 0.8:
    p, head = point_on(ctrl, pos / s[-1])
    a = head + side * tilt
    cx, cy = p + polar(1.1, a)
    m |= soften(ellipse(x, y, cx, cy, 0.6, 0.8, angle=a - 90))
    pos, side = pos + step, -side
  p, head = point_on(ctrl, 0.999)
  cx, cy = p + polar(0.65, head)
  m |= soften(ellipse(x, y, cx, cy, 0.6, 0.8, angle=head - 90))
  L.join(closing(m, FILLET))


# --- Motif groups (local mm, centred on the placement point) ------------------

def g_anemone(x, y):
  L = Layers(x.shape)
  for r, a, ang, ln, bend in [(5, 66, 62, 10.0, 0.10), (5, 168, 173, 10.0, -0.12),
                              (5, 295, 292, 10.0, 0.10)]:
    add_leaf(L, x, y, polar(r, a), ang, ln, bend=bend)
  anemone(L, x, y, 8.5)
  return L


def g_dahlia(x, y):
  L = Layers(x.shape)
  for r, a, ang, ln, bend in [(4.5, 166, 170, 10.0, 0.12),
                              (4.5, 54, 50, 9.5, -0.08), (4.5, 324, 320, 9.0, 0.10),
                              (4.5, 241, 245, 8.5, -0.10)]:
    add_leaf(L, x, y, polar(r, a), ang, ln, bend=bend)
  dahlia(L, x, y, 8.0)
  return L


def g_rose(x, y):
  L = Layers(x.shape)
  for r, a, ang, ln, bend in [(4, 160, 165, 11.5, 0.12), (4, 38, 33, 9.5, -0.10)]:
    add_leaf(L, x, y, polar(r, a), ang, ln, bend=bend)
  wild_rose(L, x, y, 7.0)
  return L


def g_tulip(x, y):
  """Tulip on a curved stem."""
  L = Layers(x.shape)
  tulip(L, x, y, 6.5)
  L.join(band(x, y, [(0, 0.5), (1.0, 3.5), (-0.75, 7.0)]))
  return L


def g_bud(x, y):
  """Bud on an S-curved stem, a leaf off the stem."""
  L = Layers(x.shape)
  ctrl = [(0, 0.9), (-0.75, 3.8), (0.75, 6.8)]
  bud(L, x, y, 5.5)
  p, head = point_on(ctrl, 0.6)
  blade, rib = leaf(x, y, tuple(p), head + 60, 7.0, bend=0.12)
  join_filleted(L, [band(x, y, ctrl), blade], [rib])
  return L


def g_bud_mirrored(x, y):
  """The bud with its stem curves and leaf on the other side."""
  return g_bud(-x, y)


def g_spray(x, y):
  """Three long leaves off one curved stem."""
  L = Layers(x.shape)
  ctrl = [(-6.0, 1.5), (-1.0, -1.5), (3.0, 0.0)]
  parts, ribs = [band(x, y, ctrl)], []
  for f, off, ln, bend in [(0.35, -42, 8.5, -0.10), (0.7, 45, 9.0, 0.10), (0.999, 0, 10.0, -0.08)]:
    p, head = point_on(ctrl, f)
    blade, rib = leaf(x, y, tuple(p), head + off, ln, bend=bend)
    parts.append(blade)
    ribs.append(rib)
  join_filleted(L, parts, ribs)
  return L


def g_sprig(x, y):
  L = Layers(x.shape)
  sprig(L, x, y, [(-5.5, 0.0), (0.0, -1.75), (5.5, 0.0)])
  return L


# Placements in tile mm: group, u, v, turn in degrees, patch half-size mm.
# Back to front: the gap motifs first, the big flowers over them.

Q0_TILE = [
  (g_spray, 21.95, 2.35, -25, 16),
  (g_bud_mirrored, 20.0, 15.0, -40, 11.5),
  (g_tulip, 5.5, 28.5, 20, 12),
  (g_sprig, 9.5, 39.0, 160, 8.5),
  (g_sprig, 35.95, 23.8, 110, 8.5),
  (g_tulip, 36.4, 37.8, 150, 13),
  (g_anemone, 7.45, 9.35, 8, 17),
  (g_dahlia, 21.95, 29.85, 0, 16.5),
  (g_rose, 35.95, 10.75, 25, 17.5),
]


# --- Repeat -------------------------------------------------------------------

def patch(tu, tv, rot, reach):
  """Pixel origin and size of the patch round tile point (tu, tv) in the
  first repeat, and its local coordinates turned by rot."""
  fu, fv = ORIGIN[0] + tu, ORIGIN[1] + tv
  r0, c0 = int(math.floor((fv - reach) * S)), int(math.floor((fu - reach) * S))
  n = int(math.ceil(2 * reach * S))
  du, dv = np.meshgrid((np.arange(c0, c0 + n) + 0.5) / S - fu, (np.arange(r0, r0 + n) + 0.5) / S - fv)
  x, y = turn(du, dv, 0, 0, rot)
  return r0, c0, n, x, y


def repeats(r0, c0, n):
  """Each repeat of an n px patch at (r0, c0) that meets the face: face
  slices and the matching patch slices."""
  cols = int(W / (TILE * S)) + 3
  rows = int(H / (TILE * S)) + cols + 3
  for i in range(-2, cols):
    for j in range(-rows, rows):
      rr = r0 + round((DROP * i + TILE * j) * S)
      cc = c0 + round(TILE * i * S)
      a0, b0, a1, b1 = max(rr, 0), max(cc, 0), min(rr + n, H), min(cc + n, W)
      if a0 < a1 and b0 < b1:
        yield ((slice(a0, a1), slice(b0, b1)),
               (slice(a0 - rr, a1 - rr), slice(b0 - cc, b1 - cc)))


def draw(placements):
  canvas = np.zeros((H, W), bool)
  for group, tu, tv, rot, reach in placements:
    r0, c0, n, x, y = patch(tu, tv, rot, reach)
    L = group(x, y)
    rim = L.shape[[0, -1], :].any() or L.shape[:, [0, -1]].any()
    assert not rim, f'{group.__name__} at ({tu}, {tv}) overruns its patch'
    halo = grow(L.shape, GAP)
    for face, local in repeats(r0, c0, n):
      canvas[face] &= ~halo[local]
      canvas[face] |= L.light[local]
  return canvas


def drop_cut_specks(light, area=5.0):
  """Remove light pieces under `area` mm2 that the window and hole
  clearances cut off a motif; flower centre discs away from the cuts stay."""
  lab, n = ndimage.label(light, structure=np.ones((3, 3)))
  cut = np.zeros(n + 1, bool)
  cut[np.unique(lab[grow(~ALLOWED, 0.15) & light])] = True
  cut &= np.bincount(lab.ravel(), minlength=n + 1) < area * S * S
  cut[0] = False
  return light & ~cut[lab]


def tile_map(placements, cell=1.0):
  """One tile at `cell` mm per character: the letter of the motif covering
  each cell centre, * where two silhouettes overlap."""
  owner = np.full((H, W), -1, int)
  for k, (group, tu, tv, rot, reach) in enumerate(placements):
    r0, c0, n, x, y = patch(tu, tv, rot, reach)
    L = group(x, y)
    for face, local in repeats(r0, c0, n):
      sub, m = owner[face], L.shape[local]
      sub[m & (sub >= 0)] = -2
      sub[m & (sub == -1)] = k
  names = 'abcdefghijklmnopqrstuvwxyz'
  rows = []
  for v in np.arange(cell / 2, TILE, cell):
    row = ''
    for u in np.arange(cell / 2, TILE, cell):
      o = owner[int((ORIGIN[1] + v) * S), int((ORIGIN[0] + u) * S)]
      row += '.' if o == -1 else '*' if o == -2 else names[o]
    rows.append(row)
  print('\n'.join(rows))
  for k, p in enumerate(placements):
    print(f'  {names[k]} {p[0].__name__} at ({p[1]}, {p[2]})')


def q0_botanical():
  return drop_cut_specks(draw(Q0_TILE) & ALLOWED)


CANDIDATES = [('Q0', q0_botanical)]


def main(args):
  if '--map' in args:
    tile_map(Q0_TILE)
    return
  cb.OUT.mkdir(parents=True, exist_ok=True)
  for name, fn in CANDIDATES:
    if args and name not in args:
      continue
    img, _ = cb.render(name, fn)
    light = np.all(np.asarray(img) == LIGHT, axis=2)
    print(f'  light covers {100 * np.count_nonzero(light & ALLOWED) / np.count_nonzero(ALLOWED):.1f} % of the field')


if __name__ == '__main__':
  main(sys.argv[1:])
