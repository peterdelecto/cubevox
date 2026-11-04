#!/usr/bin/env python3
"""Woven lid-face textures A0-A4 for the two-tone inlay.

Each candidate models its strands as a height field, lights it from the upper
left, and turns the shading into printable marks: light stripes along each
strand's lit edge, dark core bands and grain lines that swell where a strand
turns from the light, contour lines round a pillow, and cast-shadow gaps that
widen on the lower-right side. The printability pass then opens the light mask
with an 8 px disc and closes it with a 6 px disc until both leave it unchanged.

  candidates_a.py              all five candidates as <name>.png, and the report
  candidates_a.py A1 A3        just those candidates
  candidates_a.py A1 --debug   also a tone / raw / printable crop in scratch-A/
"""

import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build' / 'cane' / 'candidates'

# Face geometry from weave.py proof(), lid face coordinates in mm.

FACE = (-3.5, -148.5, 188.5, 3.5)
FIELD = (-0.5, -145.5, 185.5, 0.5)
HOLES = ([(8.5 + 24 * i, -85.0, 6.25) for i in range(8)]
         + [(8.5 + 24 * i, -108.5, 8.8) for i in range(8)]
         + [(8.5 + 24 * i, -134.5, 8.8) for i in range(8)]
         + [(8.5, -32.0, 6.25), (176.5, -31.5, 8.8), (117.94, -40.75, 7.97)])
OLED = (61.54, -53.0, 98.03, -28.0)
CLEAR = 1.0
SCALE = 10                      # px per mm
W, H = 1920, 1520
DARK = (40, 30, 24)
LIGHT = (196, 150, 96)
MIN_LIGHT_PX = 8                # 0.8 mm
MIN_DARK_PX = 6                 # 0.6 mm

_cols = (FACE[0] + (np.arange(W) + 0.5) / SCALE).astype(np.float32)
_rows = (FACE[3] - (np.arange(H) + 0.5) / SCALE).astype(np.float32)
X, Y = np.meshgrid(_cols, _rows)

# Light comes from the upper left, 45 degrees up.

ELEV = math.radians(45)


# ---------------------------------------------------------------- strands

class Rand:
  """Per-strand random values in [-1, 1], stable for any strand index."""

  def __init__(self, seed, n=4096):
    self.n = n
    self.vals = np.random.RandomState(seed).uniform(-1, 1, (16, n)).astype(np.float32)

  def __call__(self, ch, k):
    return self.vals[ch, np.mod(k, self.n)]


def profile(kind, s):
  """Cross-section height p(s) in [0, 1] and dp/ds, for s in (-1, 1)."""
  s = np.clip(s, -0.995, 0.995)
  if kind == 'round':
    p = np.sqrt(1 - s * s)
    return p, -s / p
  if kind == 'flat':                    # flat top, rounded edges
    a = np.abs(s) ** 4
    p = (1 - a) ** 0.25
    return p, -np.sign(s) * np.abs(s) ** 3 * (1 - a) ** -0.75
  if kind == 'soft':                    # leather strip, full shoulders
    q = 1 - s * s
    p = q ** 0.4
    return p, -0.8 * s * q ** -0.6
  raise ValueError(kind)


def strands(ang, c0, pitch, hw, centre, height, r, kind='round', steep=1.0,
            bow=1.0, origin=(0.0, 0.0)):
  """One family of parallel strands over the whole face.

  ang is the strand axis in degrees. Strand k's nominal centreline is
  n.p = c0 + k * pitch, with n the axis turned 90 degrees anticlockwise and p
  measured from origin. hw(k, u) is the half width, centre(k, u) the sideways
  wobble of the centreline and height(k, u) its height and slope along u, all
  in mm. r is the cross-section height the shadows see. The lighting sees the
  section as round when steep is 1; bow scales the slope along the strand.

  d is the distance across the strand from its lit edge, the edge facing the
  light; a strand square to the light takes its upper edge as the lit one.
  """
  a = math.radians(ang)
  ax, ay = math.cos(a), math.sin(a)
  nx, ny = -ay, ax
  lit = 1.0 if nx * math.cos(math.radians(115)) + ny * math.sin(math.radians(115)) > 0 else -1.0
  xr, yr = X - origin[0], Y - origin[1]
  sn = xr * nx + yr * ny - c0
  u = xr * ax + yr * ay
  k0 = np.rint(sn / pitch).astype(np.int32)
  out = None
  for dk in (-1, 0, 1):
    k = k0 + dk
    v = sn - k * pitch - centre(k, u)
    h = hw(k, u)
    s = v / h
    inside = np.abs(s) < 1
    p, dp = profile(kind, s)
    z, dz = height(k, u)
    surf = np.where(inside, z + r * p, -np.inf).astype(np.float32)
    cand = dict(surf=surf, z=z.astype(np.float32), v=v, k=k, u=u,
                d=(h - lit * v).astype(np.float32), wd=(2 * h).astype(np.float32),
                bx=(bow * dz * ax).astype(np.float32), by=(bow * dz * ay).astype(np.float32),
                gx=(steep * dp * nx + bow * dz * ax).astype(np.float32),
                gy=(steep * dp * ny + bow * dz * ay).astype(np.float32))
    if out is None:
      out = cand
    else:
      m = cand['surf'] > out['surf']
      out = {key: np.where(m, cand[key], out[key]) for key in out}
  return out


def stack(fams):
  """Top surface of several families; 'fam' says which family is on top."""
  top = dict(fams[0])
  top['fam'] = np.zeros((H, W), np.int8)
  for i, f in enumerate(fams[1:], 1):
    m = f['surf'] > top['surf']
    for key in top:
      if key != 'fam':
        top[key] = np.where(m, f[key], top[key])
    top['fam'][m] = i
  top['base'] = ~np.isfinite(top['surf'])
  top['surf'] = np.where(top['base'], 0, top['surf']).astype(np.float32)
  top['z'] = np.where(top['base'], 0, top['z']).astype(np.float32)
  top['fam'][top['base']] = -1
  return top


def woven(k, u, u0, pitch, over, amp):
  """Centre height of strand k: amp * over(k, j) at crossing u0 + j * pitch,
  cosine-eased between crossings. Returns height and slope along u."""
  t = (u - u0) / pitch
  j = np.floor(t).astype(np.int32)
  f = t - j
  o0, o1 = over(k, j), over(k, j + 1)
  e = (1 - np.cos(np.pi * f)) / 2
  z = amp * (o0 + (o1 - o0) * e)
  dz = amp * (o1 - o0) * (np.pi / (2 * pitch)) * np.sin(np.pi * f)
  return z, dz


def wobble(rnd, amp, l1, l2, ch=0):
  """Sideways wander of a centreline: two slow sines per strand."""
  def centre(k, u):
    return amp * (rnd(ch, k) * np.sin(2 * np.pi * (u / l1 + rnd(ch + 1, k)))
                  + 0.5 * rnd(ch + 2, k) * np.sin(2 * np.pi * (u / l2 + rnd(ch + 3, k))))
  return centre


def varied(rnd, hw0, per_strand, along, l, ch=4):
  """Half width that differs strand to strand and swells along each strand."""
  def hw(k, u):
    return hw0 * (1 + per_strand * rnd(ch, k)
                  + along * np.sin(2 * np.pi * (u / l + rnd(ch + 1, k))))
  return hw


# ---------------------------------------------------------------- lighting

def shift(a, dy, dx, fill):
  """out[y, x] = a[y - dy, x - dx]; cells shifted in from outside take fill."""
  out = np.full_like(a, fill)
  out[max(dy, 0):H + min(dy, 0), max(dx, 0):W + min(dx, 0)] = \
      a[max(-dy, 0):H + min(-dy, 0), max(-dx, 0):W + min(-dx, 0)]
  return out


def lambert(t, along=False, az=135.0):
  """n.L of the top surface, in [-1, 1], for light from azimuth az degrees
  (135 is the upper left) and 45 degrees up. along=True sees only the slope
  along each strand, as if its section were flat."""
  gx, gy = (t['bx'], t['by']) if along else (t['gx'], t['gy'])
  flat = math.cos(ELEV)
  lx, ly = flat * math.cos(math.radians(az)), flat * math.sin(math.radians(az))
  return (math.sin(ELEV) - gx * lx - gy * ly) / np.sqrt(1 + gx ** 2 + gy ** 2)


def cast_shadow(h, tan_e, reach_mm=4.0):
  """True where a taller surface toward the upper left blocks the light."""
  step = math.sqrt(2) / SCALE
  shadow = np.zeros(h.shape, bool)
  for m in range(1, int(reach_mm / step) + 1):
    shadow |= shift(h, m, m, 0) - m * step * tan_e > h + 1e-3
  return shadow


def crevice(z, radius_mm, dz):
  """True within radius_mm of a strand whose centre sits dz higher."""
  r = radius_mm * SCALE
  top = z.copy()
  ri = int(r)
  for dy in range(-ri, ri + 1):
    for dx in range(-ri, ri + 1):
      if (dy or dx) and dy * dy + dx * dx <= r * r:
        np.maximum(top, shift(z, dy, dx, -np.inf), out=top)
  return top > z + dz


def tone(lam, lo, hi):
  return np.clip((lam - lo) / (hi - lo), 0, 1)


# ---------------------------------------------------------------- marks

def engrave(S, d, lines):
  """Engraved lines along a strand. Each line is (centre, peak, width): its
  centre in mm from the lit edge, the tone below which it shows, its width."""
  m = S > 0
  for centre, peak, width in lines:
    m &= ~((S < peak) & (np.abs(d - centre) < width / 2))
  return m


def tube(S, t, core, s0=0.75, recede=2.0):
  """A round strand as a light stripe along its lit edge and a dark core band
  core mm wide along its far edge. Below the tone s0 the band widens toward
  the lit edge by recede mm per unit of tone, so the stripe tapers where the
  strand turns from the light."""
  body_end = t['wd'] - core - np.maximum(s0 - S, 0) * recede
  return (t['d'] < body_end) & (S > 0)


def contour(S, level, width):
  """True on a line width mm wide where the tone S passes through level."""
  gy, gx = np.gradient(S)
  slope = np.hypot(gx, gy) * SCALE              # tone per mm
  return np.abs(S - level) < slope * width / 2


# ---------------------------------------------------------------- printability

def disc(d):
  """Offsets of a d px disc (d even, centred on a pixel corner)."""
  r = d / 2
  rng = range(-d // 2, d // 2)
  return [(dy, dx) for dy in rng for dx in rng
          if (dy + 0.5) ** 2 + (dx + 0.5) ** 2 <= r * r]


def erode(m, se):
  out = np.ones_like(m)
  for dy, dx in se:
    out &= shift(m, -dy, -dx, False)
  return out


def dilate(m, se):
  out = np.zeros_like(m)
  for dy, dx in se:
    out |= shift(m, dy, dx, False)
  return out


def opening(m, d):
  se = disc(d)
  return dilate(erode(m, se), se)


def closing(m, d):
  return ~opening(~m, d)


def allowed():
  """Inside the field and clear of every hole and the OLED window."""
  m = (X > FIELD[0]) & (X < FIELD[2]) & (Y > FIELD[1]) & (Y < FIELD[3])
  for x, y, d in HOLES:
    m &= (X - x) ** 2 + (Y - y) ** 2 > (d / 2 + CLEAR) ** 2
  m &= ~((X > OLED[0] - CLEAR) & (X < OLED[2] + CLEAR)
         & (Y > OLED[1] - CLEAR) & (Y < OLED[3] + CLEAR))
  return m


def printable(mask, ok):
  """Open with the 8 px disc and close with the 6 px disc until both hold.

  Closing can bridge two light shapes with a neck under 8 px that the next
  opening cuts again. Such a neck is cut for good, with a 6 px gap.
  """
  m = mask & ok
  for _ in range(20):
    m = opening(m, MIN_LIGHT_PX)
    c = closing(m, MIN_DARK_PX) & ok
    neck = c & ~opening(c, MIN_LIGHT_PX)
    if not neck.any():
      m = c
      break
    m = c & ~dilate(neck, disc(MIN_DARK_PX))
  assert np.array_equal(opening(m, MIN_LIGHT_PX), m), 'light feature under 0.8 mm'

  # The square corners of the face and the OLED cut-out are not gaps.

  assert np.array_equal(closing(m, MIN_DARK_PX) & ok, m), 'dark gap under 0.6 mm'
  return m


# ---------------------------------------------------------------- candidates

DEBUG = {}                      # each candidate leaves its tone image here

def a0():
  """Cane webbing as lit round strands: paired horizontal and vertical strands
  with single diagonals over them, octagonal openings, on the 12 mm band lines."""
  rnd = Rand(100)
  strand, pair_gap, diag = 3.0, 0.7, 2.8
  off = (strand + pair_gap) / 2
  org = (2.5, 5.0)
  q = 12 / math.sqrt(2)
  pairs, diags = 0.6, 1.2        # a diagonal never dips into a pair
  par = lambda k, j: np.where((k + j) % 2 == 0, 1.0, -1.0).astype(np.float32)
  fams = []
  for side in (-1, 1):
    hw = varied(rnd, strand / 2, 0.04, 0.03, 37.0, ch=4 + side)
    wob = wobble(rnd, 0.10, 61.0, 23.0, ch=8 + side)

    # Horizontal band k crosses vertical band j at x = 12 j; it lies on top
    # when k + j is even. Vertical bands lie on top at the odd junctions.

    def h_height(k, u):
      z, dz = woven(k, u, 0.0, 12.0, par, 0.2)
      return z + pairs, dz

    def v_height(k, u):
      z, dz = woven(k, u, 0.0, 12.0, lambda k, j: -par(k, j), 0.2)
      return z + pairs, dz

    fams.append(strands(0, side * off, 12.0, hw, wob, h_height, 0.35, bow=4, origin=org))
    fams.append(strands(90, side * off, 12.0, hw, wob, v_height, 0.35, bow=4, origin=org))

  # Diagonals cross each other every 12/sqrt2 mm along their length; "\" lies
  # on top on the vertical band lines and "/" on the horizontal ones.

  def fwd_height(k, u):
    u0 = (k + 0.5) * q
    z, dz = woven(k, u, u0, q, lambda k, j: np.where(j % 2 == 1, 1.0, -1.0), 0.2)
    return z + diags, dz

  def back_height(k, u):
    u0 = -(k + 0.5) * q
    z, dz = woven(k, u, u0, q, lambda k, j: np.where(j % 2 == 0, 1.0, -1.0), 0.2)
    return z + diags, dz

  hwd = varied(rnd, diag / 2, 0.04, 0.03, 41.0, ch=12)
  fams.append(strands(45, q / 2, q, hwd, wobble(rnd, 0.10, 53.0, 19.0, ch=0),
                      fwd_height, 0.35, bow=4, origin=org))
  fams.append(strands(-45, q / 2, q, hwd, wobble(rnd, 0.10, 47.0, 29.0, ch=2),
                      back_height, 0.35, bow=4, origin=org))
  t = stack(fams)
  S = tone(lambert(t, along=True), 0.2, 0.83)
  S[t['base'] | cast_shadow(t['surf'], 0.5) | crevice(t['z'], 0.8,0.15)] = 0
  DEBUG['S'] = S

  # Each strand is a highlight stripe along its lit edge; a 0.75 mm core band
  # on the far side joins the cast shadow, so the gaps widen to the lower right.

  return tube(S, t, 0.75)


def a1():
  """Plain basket weave of wide flat strands that bow over and under, 6 mm
  pitch, 12 mm repeat."""
  rnd = Rand(200)
  pitch, width = 6.0, 5.1
  org = (8.5, -85.0)                 # a crossing on each slot column
  par = lambda k, j: np.where((k + j) % 2 == 0, 1.0, -1.0).astype(np.float32)
  hw = varied(rnd, width / 2, 0.05, 0.03, 43.0)
  wob = wobble(rnd, 0.15, 71.0, 27.0)

  # Vertical strand k sits at x = -6 k and horizontal strand k at y = 6 k from
  # the origin; parity flips sign-free, so k + j even puts the vertical on top.

  def v_height(k, u):
    return woven(k, u, 0.0, pitch, par, 0.45)

  def h_height(k, u):
    return woven(k, u, 0.0, pitch, lambda k, j: -par(k, j), 0.45)

  fams = [strands(0, 0.0, pitch, hw, wob, h_height, 0.3, 'flat', bow=2.0, origin=org),
          strands(90, 0.0, pitch, hw, wob, v_height, 0.3, 'flat', bow=2.0, origin=org)]
  t = stack(fams)
  S = tone(lambert(t, along=True), 0.2, 0.83)
  S[t['base'] | cast_shadow(t['surf'], 0.7) | crevice(t['z'], 0.8,0.25)] = 0
  DEBUG['S'] = S

  # A grain line down the middle and a dark band along the far edge run the
  # full length of each strand: 0.6 mm wide where it faces the light, 0.9 mm
  # where it turns away. Where it dives under the next strand it goes dark.

  wd = t['wd']
  w = np.where(S < 0.7, 0.9, 0.6)
  lines = [(wd, 2.0, 2 * w + 0.2), (wd / 2 - 0.3, 2.0, w), (wd / 2, 0.4, 2 * wd)]
  return engrave(S, t['d'], lines)


def a2():
  """Herringbone twill: narrow strands, each floating over two and under two,
  the twill turning every four strands (12 mm), so the zigzag repeats at 24 mm."""
  rnd = Rand(300)
  pitch, width = 3.0, 2.3
  org = (8.5 - 1.5, -85.0)

  def warp_over(k, j):
    up = np.where(np.floor_divide(k, 4) % 2 == 0, (j + k) % 4, (j - k) % 4)
    return np.where(up < 2, 1.0, -1.0).astype(np.float32)

  hw = varied(rnd, width / 2, 0.05, 0.04, 31.0)
  wob = wobble(rnd, 0.1, 57.0, 21.0)

  # Warps run up the face (u = y), wefts across (u = x); strand k = 0 of each
  # family is centred on the origin, crossings at u = j * pitch.

  def v_height(k, u):
    return woven(-k, u, 0.0, pitch, warp_over, 0.4)

  def h_height(k, u):
    return woven(k, u, 0.0, pitch, lambda j, kk: -warp_over(kk, j), 0.4)

  fams = [strands(0, 0.0, pitch, hw, wob, h_height, 0.3, bow=1.5, origin=org),
          strands(90, 0.0, pitch, hw, wob, v_height, 0.3, bow=1.5, origin=org)]
  t = stack(fams)
  S = tone(lambert(t, along=True), 0.2, 0.83)
  S[t['base'] | cast_shadow(t['surf'], 0.7) | crevice(t['z'], 0.8,0.25)] = 0
  DEBUG['S'] = S
  return tube(S, t, 0.7, recede=2.5)


def a3():
  """Woven leather: soft strips 7 mm wide in a diagonal plain weave, each
  crossing a pillow, 12 mm repeat across the face."""
  rnd = Rand(400)
  q = 12 / math.sqrt(2)
  width = 7.0
  org = (8.5, -85.0)
  par = lambda k, j: np.where((k + j) % 2 == 0, 1.0, -1.0).astype(np.float32)
  hw = varied(rnd, width / 2, 0.04, 0.03, 53.0)
  wob = wobble(rnd, 0.15, 67.0, 31.0)

  def fwd_height(k, u):
    return woven(k, u, 0.0, q, par, 0.8)

  def back_height(k, u):
    return woven(k, u, 0.0, q, lambda k, j: -par(j, k), 0.8)

  fams = [strands(45, 0.0, q, hw, wob, fwd_height, 0.5, 'soft', bow=1.5, origin=org),
          strands(-45, 0.0, q, hw, wob, back_height, 0.5, 'soft', bow=1.5, origin=org)]
  t = stack(fams)

  # Light from 115 degrees, still upper left, so both strip directions show
  # their pillows rather than one reading as flat bars.

  S = tone(lambert(t, az=115.0), 0.15, 0.85)
  S[t['base'] | cast_shadow(t['surf'], 0.5) | crevice(t['z'], 1.0, 0.3)] = 0
  DEBUG['S'] = S

  # Each pillow is a lit shape bounded where the tone falls to 0.3, with a
  # 0.7 mm contour line where it falls through 0.65 that sets the highlight
  # apart from the shoulder.

  return (S > 0.3) & ~contour(S, 0.65, 0.7)


def a4():
  """Loose rattan wicker: round weavers of uneven width that wander, passing
  over and under upright stakes 12 mm apart, with a node here and there."""
  rnd = Rand(500)
  pitch = 4.4
  org = (8.5, -85.0)
  stake_pitch = 12.0

  def over(j, k):
    return np.where((j + k) % 2 == 0, 1.0, -1.0).astype(np.float32)

  hw = varied(rnd, 1.7, 0.12, 0.06, 29.0)
  wob = wobble(rnd, 0.35, 47.0, 17.0)

  def weaver_height(j, u):
    z, dz = woven(j, u, 0.0, stake_pitch, over, 0.5)
    return z + 0.6, dz

  def stake_height(k, u):
    return np.full(u.shape, 0.6, np.float32), np.zeros(u.shape, np.float32)

  stake_hw = varied(rnd, 1.7, 0.06, 0.04, 23.0, ch=10)
  fams = [strands(0, 0.0, pitch, hw, wob, weaver_height, 0.35, bow=2.0, origin=org),
          strands(90, 0.0, stake_pitch, stake_hw, wobble(rnd, 0.2, 37.0, 13.0, ch=12),
                  stake_height, 0.45, origin=org)]
  t = stack(fams)
  S = tone(lambert(t, along=True), 0.2, 0.83)
  S[t['base'] | cast_shadow(t['surf'], 0.6) | crevice(t['z'], 0.8,0.25)] = 0

  # Nodes: a dark ring across a weaver every 25 to 60 mm.

  weaver = t['fam'] == 0
  k = t['k']
  spacing = 42 + 17 * rnd(14, k)
  pos = np.mod(t['u'] - 30 * rnd(15, k), spacing)
  S[weaver & (pos < 0.7)] = 0
  DEBUG['S'] = S

  # A lit body and a 0.7 mm core band along the far edge; the band eats into
  # the body where the rod arcs away from the light, so each run tapers.

  return tube(S, t, 0.7, s0=0.85, recede=4.0)


CANDIDATES = {
  'A0': (a0, 'cane webbing as round strands: paired 3.0 mm horizontal and vertical strands '
             '0.7 mm apart, 2.8 mm diagonals over them, octagonal openings, 12 mm repeat'),
  'A1': (a1, 'plain over-under basket weave of 5.1 mm flat strands on a 6 mm pitch that bow '
             'over one and under the next, 12 mm repeat'),
  'A2': (a2, 'herringbone twill of 2.3 mm strands on a 3 mm pitch, each floating over two '
             'and under two, the twill turning every 12 mm, so the zigzag repeats at 24 mm'),
  'A3': (a3, 'woven leather: 7.0 mm soft strips in a diagonal plain weave, each crossing a '
             'pillow, 12 mm repeat across the face'),
  'A4': (a4, 'loose rattan wicker: round weavers 3.4 mm wide (12 % strand to strand) on a '
             '4.4 mm pitch, wandering up to 0.5 mm, over and under 3.4 mm stakes 12 mm apart, '
             'a node every 25 to 59 mm'),
}


# ---------------------------------------------------------------- output

def to_image(m):
  img = np.empty((H, W, 3), np.uint8)
  img[:] = DARK
  img[m] = LIGHT
  return Image.fromarray(img)


def zoom(img):
  """60 x 60 mm from the centre of the field, scaled 2x."""
  cx, cy = (FIELD[0] + FIELD[2]) / 2, (FIELD[1] + FIELD[3]) / 2
  c0 = round((cx - 30 - FACE[0]) * SCALE)
  r0 = round((FACE[3] - (cy + 30)) * SCALE)
  return img.crop((c0, r0, c0 + 600, r0 + 600)).resize((1200, 1200), Image.NEAREST)


def scratch(name, raw, final):
  """Tone, raw marks and printable marks of a clear 30 mm square, 3x."""
  box = (1285, 535, 1585, 835)
  grey = Image.fromarray((DEBUG['S'] * 255).astype(np.uint8)).convert('RGB')
  tiles = [grey.crop(box), to_image(raw).crop(box), to_image(final).crop(box)]
  out = Image.new('RGB', (2700, 900))
  for i, tile in enumerate(tiles):
    out.paste(tile.resize((900, 900), Image.NEAREST), (900 * i, 0))
  (OUT / 'scratch-A').mkdir(exist_ok=True)
  out.save(OUT / 'scratch-A' / f'{name}.png')


def build(name, ok, debug=False):
  fn, _ = CANDIDATES[name]
  raw = fn() & ok
  final = printable(raw, ok)
  changed = np.count_nonzero(raw ^ final) / max(np.count_nonzero(raw), 1)
  img = to_image(final)
  img.save(OUT / f'{name}.png')
  if debug:
    scratch(name, raw, final)
  colours = len(img.getcolors(4) or [None] * 5)
  print(f'{name}: pass changed {changed * 100:.2f} % of light pixels, '
        f'light cover {np.count_nonzero(final) / np.count_nonzero(ok) * 100:.1f} %, '
        f'{colours} colours')
  return changed


HOW = {
  'A0': 'each strand is a light highlight stripe along its lit edge with a 0.75 mm dark core '
        'band along its far edge that widens by 0.2 mm for every 10 % the shading falls below '
        '75 % where the strand dives, so pieces taper; an over '
        'strand leaves a 0.8 mm gap on its upper-left side and casts a solid shadow (light '
        '27 degrees up) on its lower-right side, which joins the core band into a wide gap',
  'A1': 'a grain line down the middle of each strand and a dark band along its far edge are '
        '0.6 mm wide where the strand rises toward the light and 0.9 mm where it turns away, '
        'and the strand goes solid dark where it dives; 0.8 mm gaps round each over strand, '
        'solid cast shadows (light 35 degrees up) on the lower right',
  'A2': 'each float is a light capsule along its lit edge with a 0.7 mm core band along the '
        'far edge; toward the diving end the band widens by 0.25 mm for every 10 % the shading '
        'falls below 75 %, so floats taper; '
        '0.8 mm gaps round each over strand, solid cast shadows (light 35 degrees up) on '
        'the lower right',
  'A3': 'each pillow is lit from 115 degrees (upper left) and drawn as a solid light shape cut '
        'where the shading falls to 30 %, with a 0.7 mm contour line where it passes 65 % that '
        'sets the highlight apart from the shoulder; 1.0 mm deep gaps round each over strip, '
        'solid cast shadows (light 27 degrees up) on the lower right',
  'A4': 'each weaver and stake is a light stripe along its lit edge with a 0.7 mm core band '
        'along the far edge that widens by 0.4 mm for every 10 % the shading falls below 85 % '
        'where the weaver arcs away from the light, so each run tapers like a brush stroke; '
        '0.7 mm dark node rings; 0.8 mm gaps '
        'round each over piece, solid cast shadows (light 31 degrees up) on the lower right',
}


def report(changes):
  lines = []
  for name, ch in changes.items():
    lines.append(f'{name}: {CANDIDATES[name][1]}; {HOW[name]}; '
                 f'the printability pass changed {ch * 100:.2f} % of light pixels.')
  (OUT / 'REPORT-A.md').write_text('\n'.join(lines) + '\n')


def main(argv):
  debug = '--debug' in argv
  argv = [a for a in argv if a != '--debug']
  names = argv or list(CANDIDATES)
  ok = allowed()
  changes = {name: build(name, ok, debug) for name in names}
  if not argv:
    report(changes)


if __name__ == '__main__':
  main(sys.argv[1:])
