#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family V: small icon florals.

Five two-colour florals with flowers 8 to 13 mm across. Each flower is a flat
light silhouette split by dark lines: separate petals, or petals parted by
dark divides, round a visible centre. Each candidate repeats one tile of
placed motifs in half-drop: each column of tiles sits half a tile below the
column to its left. Motifs paint back to front, and each clears a dark halo
round itself in what lies beneath, so a flower over a stem reads as the stem
tucked behind it. Drawing tools follow candidates_q; the face grid, print
pass and report come from candidates_b.

  candidates_v.py           every candidate as <name>.png
  candidates_v.py V0 V3     only the named candidates
  candidates_v.py --map V0  which motif covers each 1 mm cell of a 60 mm window,
                            * where two motifs come within 1 mm; light share before printing
"""

import math
import sys
from functools import partial

import numpy as np

import candidates_b as cb
import candidates_q as cq
from candidates_b import ALLOWED, H, LIGHT, S, W, closing, disc
from candidates_q import Layers, band, bezier, capsule, ellipse, grow, leaf, lobes, point_on, polar, soften, turn

# Dark marks print at 0.6 mm or wider. Lines and rings drawn 0.65 mm wide and
# halos of cq.GAP (0.7 mm) survive the print pass at every angle.

LINE = 0.65              # dark line cut through a shape, mm
RING = 0.65              # dark ring round a flower's light centre, mm
STEM = 1.0               # light stem, branch and twig width, mm
FILLET = disc(8)         # closes dark corners under 0.8 mm where stems, leaves and heads join
NOTCH = disc(7)          # rounds the dark notches between leaf lobes
SPECK = 4.0              # light pieces under this, mm2, cut off by the window and holes are dropped


# --- Repeat -------------------------------------------------------------------

class Tile:
  """A w x h mm repeat in half-drop, its top-left corner at face (u, v) mm."""

  def __init__(self, w, h, origin):
    self.w, self.h, self.drop, self.origin = w, h, h / 2, origin


def patch(tile, tu, tv, rot, reach):
  """Pixel origin and size of the patch round tile point (tu, tv), and its
  local coordinates turned by rot degrees."""
  fu, fv = tile.origin[0] + tu, tile.origin[1] + tv
  r0, c0 = int(math.floor((fv - reach) * S)), int(math.floor((fu - reach) * S))
  n = int(math.ceil(2 * reach * S))
  du, dv = np.meshgrid((np.arange(c0, c0 + n) + 0.5) / S - fu, (np.arange(r0, r0 + n) + 0.5) / S - fv)
  x, y = turn(du, dv, 0, 0, rot)
  return r0, c0, n, x, y


def repeats(tile, r0, c0, n):
  """Each repeat of an n px patch at (r0, c0) that meets the face: face
  slices and the matching patch slices."""
  for i in range(math.floor(-(c0 + n) / (tile.w * S)), math.ceil((W - c0) / (tile.w * S)) + 1):
    lo = math.floor((-(r0 + n) / S - tile.drop * i) / tile.h)
    hi = math.ceil(((H - r0) / S - tile.drop * i) / tile.h)
    for j in range(lo, hi + 1):
      rr = r0 + round((tile.drop * i + tile.h * j) * S)
      cc = c0 + round(tile.w * i * S)
      a0, b0, a1, b1 = max(rr, 0), max(cc, 0), min(rr + n, H), min(cc + n, W)
      if a0 < a1 and b0 < b1:
        yield ((slice(a0, a1), slice(b0, b1)),
               (slice(a0 - rr, a1 - rr), slice(b0 - cc, b1 - cc)))


def motifs(tile, placements):
  """Each placement drawn on its patch: the Layers and the patch origin and size."""
  for group, tu, tv, rot, reach in placements:
    r0, c0, n, x, y = patch(tile, tu, tv, rot, reach)
    L = group(x, y)
    rim = L.shape[[0, -1], :].any() or L.shape[:, [0, -1]].any()
    name = getattr(group, 'func', group).__name__
    assert not rim, f'{name} at ({tu}, {tv}) overruns its patch'
    yield L, r0, c0, n


def draw(tile, placements):
  canvas = np.zeros((H, W), bool)
  for L, r0, c0, n in motifs(tile, placements):
    halo = grow(L.shape, cq.GAP)
    for face, local in repeats(tile, r0, c0, n):
      canvas[face] &= ~halo[local]
      canvas[face] |= L.light[local]
  return cq.drop_cut_specks(canvas & ALLOWED, area=SPECK)


def tile_map(tile, placements, window=(30.0, 20.0, 60.0, 60.0)):
  """Motif letters over a window of the face at 1 mm a character; * where two
  motifs (or two repeats of one) come within 1 mm."""
  owner = np.full((H, W), -1, np.int64)
  close = np.zeros((H, W), bool)
  for k, (L, r0, c0, n) in enumerate(motifs(tile, placements)):
    m = grow(L.shape, 0.5)
    for rep, (face, local) in enumerate(repeats(tile, r0, c0, n)):
      sub, mm = owner[face], m[local]
      close[face] |= mm & (sub >= 0)
      sub[mm] = k
  u0, v0, du, dv = window
  names = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'
  for v in np.arange(v0 + 0.5, v0 + dv, 1.0):
    row = ''
    for u in np.arange(u0 + 0.5, u0 + du, 1.0):
      r, c = int(v * S), int(u * S)
      o = owner[r, c]
      row += '*' if close[r, c] else '.' if o < 0 else names[o % len(names)]
    print(row)


# --- Shared shapes --------------------------------------------------------------

def to_rays(x, y, n, phase):
  """Distance from each pixel to the nearest of n rays from the origin at
  phase + k * 360 / n degrees."""
  step = 360 / n
  a = (np.degrees(np.arctan2(y, x)) - phase) / step
  return np.hypot(x, y) * np.sin(np.radians(np.abs(a - np.round(a)) * step))


def centre(L, x, y, disc_r, dot=0.0):
  """A light disc of radius disc_r inside one dark RING, with an optional
  dark dot `dot` mm across in its middle."""
  r = np.hypot(x, y)
  ring = r < disc_r + RING
  L.light &= ~ring
  L.shape |= ring
  L.light |= (r < disc_r) & (r >= dot / 2)


def radial_petals(x, y, n, r0, r1, half_w, phase=0.0):
  """n separate oval petals from radius r0 to r1, half_w mm half-width, on
  axes at phase + k * 360 / n degrees, with at least LINE of dark between
  neighbours."""
  mid, half_l = (r0 + r1) / 2, (r1 - r0) / 2
  m = np.zeros(x.shape, bool)
  for k in range(n):
    a = phase + 360 * k / n
    cx, cy = polar(mid, a)
    m |= ellipse(x, y, cx, cy, half_w, half_l, angle=a - 90)
  return soften(m & (to_rays(x, y, n, phase + 180 / n) >= LINE / 2))


def stroke(x, y, ctrl):
  """A dark LINE along the curve on ctrl."""
  pts, _ = bezier(ctrl)
  return cq.near(x, y, pts, LINE)[0] < LINE / 2


def joined(L, parts, dark=()):
  """Paint parts merged as one light shape, dark corners under 0.8 mm
  filleted, then cut the dark lines through it."""
  m = closing(np.logical_or.reduce(parts), FILLET)
  L.join(m, m & ~np.logical_or.reduce(list(dark))) if dark else L.join(m)


def g_leaf(x, y, length=4.4, width=0.42):
  """Small plain leaf from the origin toward +x."""
  L = Layers(x.shape)
  blade, _ = leaf(x, y, (0.0, 0.0), 0.0, length, width=width)
  L.over(blade)
  return L


# --- V0: daisies ------------------------------------------------------------------

def daisy(L, x, y, R=5.6, disc_r=1.5, half_w=1.26):
  """Eight separate oval petals round a light centre disc in a dark ring."""
  r = np.hypot(x, y)
  L.over(r < R, radial_petals(x, y, 8, disc_r + RING + 0.05, R, half_w, phase=22.5))
  centre(L, x, y, disc_r)


def g_daisy(x, y, stem=90.0, side=1):
  """Daisy on a short stem leaving toward `stem` degrees, one leaf off the
  stem on `side`."""
  L = Layers(x.shape)
  ctrl = [polar(4.5, stem), polar(7.5, stem - 7 * side), polar(10.3, stem + 5 * side)]
  p, head = point_on(ctrl, 0.5)
  blade, rib = leaf(x, y, tuple(p), head + 48 * side, 6.2, bend=0.1 * side, width=0.52)
  joined(L, [band(x, y, ctrl), blade], [rib])
  daisy(L, x, y)
  return L


def g_bud(x, y, side=1):
  """6 mm bud, tip toward -y, on a short curved stem with one leaf; the group
  centred near the origin."""
  L = Layers(x.shape)
  yb = y - 1.2
  cq.bud(L, x, yb, 6.0)
  ctrl = [(0, 0.5), (0.9 * side, 2.3), (0.3 * side, 4.2)]
  p, head = point_on(ctrl, 0.55)
  blade, rib = leaf(x, yb, tuple(p), head - 55 * side, 4.8, bend=-0.1 * side, width=0.46)
  joined(L, [band(x, yb, ctrl), blade], [rib])
  return L


# Daisies sit on the lattice (18, 9), (0, 18), which holds the half-drop
# repeat; each stem runs into one of the triangular gaps between daisies and
# a bud fills each of the other gaps. Tile mm: u, v, stem angle, leaf side,
# petal turn; buds: u, v, tip angle, leaf side.

V0_TILE = Tile(36.0, 36.0, origin=(1.0, 2.0))
V0_DAISIES = [(0.4, 0.3, 56, 1, 8), (-0.6, 18.4, 180, -1, -12),
              (18.5, 8.6, 124, 1, 15), (17.6, 27.4, 0, -1, 3)]
V0_BUDS = [(7.0, 26.2, -56, 1), (24.6, 17.6, 0, 1), (10.6, 2.2, 124, -1), (31.0, 9.2, 0, 1)]


def v0_placements():
  out = []
  for u, v, tip, side in V0_BUDS:
    out.append((partial(g_bud, side=side), u, v, tip + 90, 9.0))
  for u, v, stem, side, rot in V0_DAISIES:
    out.append((partial(g_daisy, stem=stem - rot, side=side), u, v, rot, 13.0))
  return out


def v0_daisies():
  return draw(V0_TILE, v0_placements())


# --- V1: tulips -----------------------------------------------------------------

def tulip_head(x, y, h=9.8, hw=4.2, dip=0.3):
  """Side view, base at the origin, opening toward -y: a round-bottomed cup
  whose rim rises in three points, and a dark divide down from each valley
  either side of the middle point. Returns the cup and the divides."""
  t = -y / h
  low = 0.5
  w = np.where(t < low, hw * np.sqrt(np.clip(1 - ((low - t) / low) ** 2, 0, 1)),
               hw * (1 + 0.06 * (t - low) / (1 - low)))
  top = 1.06 * hw
  rim = -h + dip * h * np.abs(np.sin(np.pi * x / top))
  cup = soften((np.abs(x) < w) & (t > 0) & (y > rim))
  dark = np.zeros(x.shape, bool)
  for side in (-1, 1):
    dark |= stroke(x, y, [(side * top / 2, -h + dip * h + 0.2), (side * 0.58 * top, -0.5 * h),
                          (side * 0.3 * top, -0.2 * h)])
  return cup, dark


def slim_leaf(x, y, base, angle, length, bend=0.0, width=0.38):
  """Narrow leaf from base toward `angle` degrees, stem-wide at its foot and
  widest past its middle, so it parts from the stem it springs from. Its
  spine bows sideways by bend * length."""
  a = math.radians(angle)
  ux, uy = math.cos(a), math.sin(a)
  bx, by = base
  ctrl = [(bx, by),
          (bx + 0.5 * length * ux - bend * length * uy, by + 0.5 * length * uy + bend * length * ux),
          (bx + length * ux, by + length * uy)]
  pts, s = bezier(ctrl)
  f = s / s[-1]
  half = 0.5 * width * length * np.sin(np.pi * f ** 1.3)
  half = np.where(f < 0.5, np.maximum(half, STEM / 2), half)
  d, i = cq.near(x, y, pts, half.max())
  return soften(d < np.append(half, 0.0)[i])


def g_tulip(x, y, lean=0.0, stem=14.0):
  """Tulip leaning `lean` degrees on an upright stem, a narrow leaf sweeping
  out and up each side, one from the stem foot and one higher; the head's
  base at the origin."""
  L = Layers(x.shape)
  hx, hy = turn(x, y, 0, 0, lean)
  cup, divides = tulip_head(hx, hy)
  bow = -math.copysign(0.6, lean) if lean else 0.0
  parts = [cup, band(x, y, [(0, -1.0), (bow, stem / 2), (0, stem)])]
  parts.append(slim_leaf(x, y, (0, stem - 0.3), -135, 8.0, bend=-0.1))
  parts.append(slim_leaf(x, y, (0, stem - 2.8), -45, 7.5, bend=0.1))
  joined(L, parts, [divides])
  return L


# Rows 26 mm apart, tulips 12 mm apart along a row, alternate rows shifted
# half a step. Leans alternate along each row. The half-drop column step
# (12, 52) lands each row on the row two below with the lean flipped, so
# the tile holds four rows of one tulip.

V1_TILE = Tile(12.0, 104.0, origin=(3.0, 11.0))
V1_ROWS = [(0.0, 0.0, -8), (6.0, 26.0, 8), (0.0, 52.0, 8), (6.0, 78.0, -8)]


def v1_placements():
  return [(partial(g_tulip, lean=lean), u, v, 0, 15.0) for u, v, lean in V1_ROWS]


def v1_tulips():
  return draw(V1_TILE, v1_placements())


# --- V2: cherry blossom ---------------------------------------------------------

def blossom(L, x, y, R=5.5, phase=-90.0):
  """Five heart-notched petals parted by dark divides, round a dark ring
  with a light dot."""
  shape, to_divide, _, r = lobes(x, y, 5, R, 0.6 * R, phase=phase)
  for k in range(5):
    bx, by = polar(R + 0.1, phase + 72 * k)
    shape &= ~(np.hypot(x - bx, y - by) < 0.75)
  L.over(r < R, soften(shape & (to_divide >= LINE / 2) & (r >= 1.35)))
  L.light |= r < 0.65


def cherry_bud(x, y, h=4.0):
  """Closed bud, tip toward -y, base at the origin."""
  t = np.clip(-y / h, 0, 1)
  return soften((np.abs(x) < 0.36 * h * np.sin(np.pi * t ** 0.65)) & (y < 0) & (y > -h))


def g_spray(x, y, branch, blossoms, buds, flip=1):
  """Blossoms and buds on twigs off one branch running along local x;
  flip=-1 mirrors the spray across its branch."""
  L = Layers(x.shape)
  y = flip * y
  parts = [band(x, y, branch)]
  for f, side, ang in buds:
    p, head = point_on(branch, f)
    a = head + side * ang
    end = p + np.array(polar(2.8, a))
    parts.append(capsule(x, y, tuple(p), tuple(end), STEM))
    parts.append(cherry_bud(*turn(x, y, end[0], end[1], a + 90)))
  for f, cx, cy, _ in blossoms:
    p, _ = point_on(branch, f)
    parts.append(capsule(x, y, tuple(p), (cx, cy), STEM))
  joined(L, parts)
  for _, cx, cy, phase in blossoms:
    blossom(L, x - cx, y - cy, phase=phase)
  return L


# One spray: branch, blossoms (attach fraction, centre, petal turn) and buds
# (attach fraction, side, angle off the branch).

SPRAY_BRANCH = [(-18.0, 0.5), (-6.0, -1.5), (6.0, 1.5), (16.0, 0.0)]
SPRAY_BLOSSOMS = [(0.2, -11.0, 2.4, -60), (0.33, -3.0, -7.2, -85), (0.55, 4.2, 7.2, -100),
                  (0.74, 10.5, -6.8, -75), (1.0, 18.2, 2.6, -95)]
SPRAY_BUDS = [(0.08, -1, 55), (0.40, 1, 90)]

# Sprays rise at 56.3 degrees along the repeat's (48, -72) direction, two to
# a tile, one mirrored, so each diagonal is a row of sprays 43.25 mm apart
# and the diagonals stand 26.6 mm apart.

V2_TILE = Tile(48.0, 48.0, origin=(2.0, 4.0))
V2_TURN = -math.degrees(math.atan2(72, 48))


def v2_placements():
  spray = partial(g_spray, branch=SPRAY_BRANCH, blossoms=SPRAY_BLOSSOMS, buds=SPRAY_BUDS)
  return [(spray, 12.0, 30.0, V2_TURN, 27.0), (partial(spray, flip=-1), 36.0, 42.0, V2_TURN, 27.0)]


def v2_cherry():
  return draw(V2_TILE, v2_placements())


# --- V3: sunflowers on a trellis --------------------------------------------------

def sunflower(L, x, y, R=6.0, disc_r=2.5):
  """Twelve pointed petals round a light centre disc in a dark ring, a dark
  dot in its middle."""
  r = np.hypot(x, y)
  shape, to_divide, _, _ = lobes(x, y, 12, R, R - 1.7, phase=15, point=0.9)
  L.over(r < R, soften(shape & (to_divide >= LINE / 2) & (r >= disc_r + RING)))
  centre(L, x, y, disc_r, dot=1.2)


def lobed_leaf(x, y, base, angle, size=7.0):
  """Leaf `size` mm long from base toward `angle` degrees: two round lobes a
  side and a round tip on a short stalk, a dark midrib. Returns the leaf
  and the rib."""
  X, Y = turn(x, y, base[0], base[1], angle)
  k = size / 7.0
  m = (np.abs(Y) < STEM / 2) & (X > -0.2) & (X < 1.6 * k)
  m |= ellipse(X / k, Y / k, 3.6, 0.0, 0.85, 2.9, angle=-90)
  for cx, cy, rr in [(2.3, 1.2, 1.2), (4.4, 0.9, 0.95)]:
    m |= np.hypot(X / k - cx, Y / k - cy) < rr
    m |= np.hypot(X / k - cx, Y / k + cy) < rr
  m |= np.hypot(X / k - 6.1, Y / k) < 0.8
  m = soften(closing(m, NOTCH))
  rib = capsule(X, Y, (1.9 * k, 0.0), (4.8 * k, 0.0), LINE)
  return m, rib


def g_trellis(x, y, length):
  """One trellis stem along local x, a lobed leaf each side near its middle,
  the two leaning apart so neither crosses the stem."""
  L = Layers(x.shape)
  half = length / 2
  parts, dark = [capsule(x, y, (-half - 0.5, 0), (half + 0.5, 0), STEM)], []
  for pos, ang in [(-1.8, 62), (1.8, 242)]:
    blade, rib = lobed_leaf(x, y, (pos, 0.0), ang)
    parts.append(blade)
    dark.append(rib)
  joined(L, parts, dark)
  return L


def g_sunflower(x, y):
  L = Layers(x.shape)
  sunflower(L, x, y)
  return L


# Nodes on the lattice (25.33, 0), (12.67, 19), which holds the half-drop
# repeat: diamonds 25.3 mm wide and 38 mm tall, three nodes a tile. A
# sunflower sits on each node; stems run node to node.

V3_TILE = Tile(38.0, 38.0, origin=(9.0, 6.0))
V3_STEP = (38.0 / 3, 19.0)


def v3_placements():
  a, b = V3_STEP
  length = math.hypot(a, b)
  ang = math.degrees(math.atan2(b, a))
  nodes = [(0.0, 0.0), (2 * a, 0.0), (a, b)]
  out = []
  for nu, nv in nodes:
    out.append((partial(g_trellis, length=length), nu + a / 2, nv + b / 2, ang, 14.0))
    out.append((partial(g_trellis, length=length), nu + a / 2, nv - b / 2, -ang, 14.0))
  for k, (nu, nv) in enumerate(nodes):
    out.append((g_sunflower, nu, nv, 7.0 * k, 7.0))
  return out


def v3_sunflowers():
  return draw(V3_TILE, v3_placements())


# --- V4: mixed ditsy ------------------------------------------------------------

def g_blossom5(x, y):
  """Five round petals parted by dark divides round a light centre in a dark ring, 10 mm."""
  L = Layers(x.shape)
  shape, to_divide, _, r = lobes(x, y, 5, 5.0, 0.62 * 5.0, phase=-90)
  L.over(r < 5.0, soften(shape & (to_divide >= LINE / 2)))
  centre(L, x, y, 0.8)
  return L


def g_star6(x, y):
  """Six pointed petals parted by dark divides round a light dot in a dark ring, 10 mm."""
  L = Layers(x.shape)
  R = 5.0
  shape, to_divide, _, r = lobes(x, y, 6, R, 0.46 * R, phase=-90, point=1.15)
  L.over(shape | (r < 0.46 * R + 0.3), soften(shape & (to_divide >= LINE / 2)))
  centre(L, x, y, 0.85)
  return L


def bell(x, y, h=7.2, hw=3.5, dip=0.32):
  """Side view, hanging, top at the origin, mouth toward +y: a dome flaring
  to a rim of three points, a dark divide up from each valley. Returns the
  bell and the divides."""
  dome = 0.35 * h
  w_top = 0.7 * hw
  w = np.where(y < dome, w_top * np.sqrt(np.clip(1 - ((dome - y) / dome) ** 2, 0, 1)),
               w_top + (hw - w_top) * (y - dome) / (h - dome))
  rim = h - dip * h * np.abs(np.sin(np.pi * x / hw))
  body = soften((np.abs(x) < w) & (y > 0) & (y < rim))
  dark = np.zeros(x.shape, bool)
  for side in (-1, 1):
    dark |= stroke(x, y, [(side * hw / 2, h - dip * h - 0.2), (side * 0.42 * hw, 0.66 * h),
                          (side * 0.36 * hw, 0.5 * h)])
  return body, dark


def g_bell(x, y):
  """Bell flower hung from a hooked stem with a small leaf, 10 mm tall; the
  group centred near the origin."""
  L = Layers(x.shape)
  xb, yb = x + 0.8, y + 2.2
  body, dark = bell(xb, yb)
  hook = [(0.0, 0.8), (0.0, -2.2), (2.4, -3.0)]
  blade, _ = leaf(xb, yb, (2.0, -2.95), -15, 3.8, width=0.44)
  joined(L, [body, band(xb, yb, hook), blade], [dark])
  return L


def g_daisy10(x, y):
  """Ten separate oval petals round a light centre disc in a dark ring, 10 mm."""
  L = Layers(x.shape)
  r = np.hypot(x, y)
  L.over(r < 5.0, radial_petals(x, y, 10, 2.2, 5.0, 0.7))
  centre(L, x, y, 1.5)
  return L


# Flowers on rows 12 mm apart, 12 mm along a row, alternate rows shifted half
# a step, each nudged up to 1.2 mm. Kind (i + 2j) mod 4 differs between every
# pair of neighbours and holds the half-drop repeat, which moves four steps
# along and two rows down. Each triangular gap between three flowers gets one
# small leaf tucked behind one of them.

V4_TILE = Tile(48.0, 48.0, origin=(0.5, 1.5))
V4_KINDS = [(g_blossom5, 5.0, 180), (g_star6, 5.0, 180), (g_bell, 5.0, 22), (g_daisy10, 5.0, 180)]


def v4_cell(i, j):
  """Tile position, kind and canonical index of flower (i, j) on the infinite grid."""
  k = i // 4
  i0, j1 = i - 4 * k, j - 2 * k
  m = j1 // 4
  j0 = j1 - 4 * m
  u = 12 * i0 + 6 * (j0 % 2) + 6 + 2.4 * (cb.hash01(i0, j0, 1) - 0.5) + 48 * k
  v = 12 * j0 + 6 + 2.4 * (cb.hash01(i0, j0, 2) - 0.5) + 24 * k + 48 * m
  return float(u), float(v), (i0 + 2 * j0) % 4, (i0, j0)


def v4_placements():
  leaves, flowers = [], []
  for j in range(4):
    for i in range(4):
      u, v, kind, _ = v4_cell(i, j)
      group, _, swing = V4_KINDS[kind]
      flowers.append((group, u, v, swing * (2 * cb.hash01(i, j, 3) - 1), 8.0))

      # The gap below (i, j) and (i + 1, j), and the gap above them.

      below = (i, j), (i + 1, j), (i + (j % 2), j + 1)
      above = (i, j), (i + 1, j), (i + (j % 2), j - 1)
      for n, gap in enumerate((below, above)):
        pts = [v4_cell(a, b) for a, b in gap]
        gu, gv = np.mean([p[0] for p in pts]), np.mean([p[1] for p in pts])
        du, dv, kind_d, _ = pts[int(3 * cb.hash01(i, j, 4 + n))]
        a = math.degrees(math.atan2(gv - dv, gu - du)) + 24 * (cb.hash01(i, j, 6 + n) - 0.5)
        bu, bv = np.array([du, dv]) + polar(V4_KINDS[kind_d][1] - 1.2, a)
        leaves.append((g_leaf, float(bu), float(bv), a, 5.0))
  return leaves + flowers


def v4_ditsy():
  return draw(V4_TILE, v4_placements())


# --- Output -------------------------------------------------------------------

CANDIDATES = [('V0', v0_daisies, V0_TILE, v0_placements),
              ('V1', v1_tulips, V1_TILE, v1_placements),
              ('V2', v2_cherry, V2_TILE, v2_placements),
              ('V3', v3_sunflowers, V3_TILE, v3_placements),
              ('V4', v4_ditsy, V4_TILE, v4_placements)]


def share(light):
  return 100 * np.count_nonzero(light & ALLOWED) / np.count_nonzero(ALLOWED)


def main(args):
  if '--map' in args:
    for name, fn, tile, places in CANDIDATES:
      if name in args and places:
        tile_map(tile, places())
        print(f'{name}: light {share(fn()):.1f} % of the field before printing')
    return
  cb.OUT.mkdir(parents=True, exist_ok=True)
  for name, fn, _, _ in CANDIDATES:
    if args and name not in args:
      continue
    img, _ = cb.render(name, fn)
    light = np.all(np.asarray(img) == LIGHT, axis=2)
    print(f'  light covers {share(light):.1f} % of the field')


if __name__ == '__main__':
  main(sys.argv[1:])
