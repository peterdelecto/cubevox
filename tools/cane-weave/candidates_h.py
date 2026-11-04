#!/usr/bin/env python3
"""Candidate lid-face inlay textures, family H: star fields, no suns, moons or planets.

The dark lid is space and the light inlay is the stars. Depth comes from
brightness and density falloff: a star's brightness sets its dot size, line
width or spike length, and the stars thin out away from where the sky is
richest. Each candidate function returns the light-inlay mask on the face
grid of candidates_b.py: 10 px/mm, 1920 x 1520 px, row 0 at the top edge of
the face. Every mark is drawn at least MIN_W wide and kept GAP from every
other mark, so the printability pass has little left to change.

  candidates_h.py           every candidate as <name>.png; prints the report lines
  candidates_h.py H0 H3     only the named candidates
"""

import math
import sys
import time

import numpy as np
from PIL import Image

from candidates_b import (CLEAR, FIELD, H, HOLES, OLED, OUT, S, U, V, W, blur, dilate, disc,
                          equalize, face_image, hash01, line_dist, printable, to_uv, wave_noise)

# Narrowest light mark and dark gap drawn, mm: the 0.8 and 0.6 mm print
# limits plus a margin for pixel steps on curves and diagonals.

MIN_W = 1.0
GAP = 0.8


# --- Where marks may go --------------------------------------------------------

def free_map():
  """mm from each pixel to the nearest place inlay may not go (the field
  edge, a clearance ring, the OLED window's clearance); negative inside."""
  u0, v0 = to_uv(FIELD[0], FIELD[3])
  u1, v1 = to_uv(FIELD[2], FIELD[1])
  d = np.minimum.reduce([U - u0, u1 - U, V - v0, v1 - V])
  for x, y, dia in HOLES:
    hu, hv = to_uv(x, y)
    d = np.minimum(d, np.hypot(U - hu, V - hv) - dia / 2 - CLEAR)
  ou0, ov0 = to_uv(OLED[0], OLED[3])
  ou1, ov1 = to_uv(OLED[2], OLED[1])
  ou0, ov0, ou1, ov1 = ou0 - CLEAR, ov0 - CLEAR, ou1 + CLEAR, ov1 + CLEAR
  ex = np.maximum(np.maximum(ou0 - U, U - ou1), 0)
  ey = np.maximum(np.maximum(ov0 - V, V - ov1), 0)
  inside = np.minimum.reduce([U - ou0, ou1 - U, V - ov0, ov1 - V])
  return np.minimum(d, np.where((ex > 0) | (ey > 0), np.hypot(ex, ey), -inside))


FREE = free_map()


def at(raster, u, v):
  """The raster's value at the pixel holding (u, v) mm; None off the face."""
  i, j = int(u * S), int(v * S)
  if 0 <= i < W and 0 <= j < H:
    return raster[j, i]
  return None


def fits_face(discs, blocked=None):
  """Every disc (u, v, r) lies wholly on the inlay field, clear of the holes
  and the OLED window, and clear of the `blocked` raster."""
  for u, v, r in discs:
    f = at(FREE, u, v)
    if f is None or f < r + 0.15:
      return False
    if blocked is not None:
      for k in range(13):
        a = k * 2 * math.pi / 12
        rr = r if k < 12 else 0.0
        if at(blocked, u + rr * math.cos(a), v + rr * math.sin(a)):
          return False
  return True


class Room:
  """The marks placed so far, each covered by discs (u, v, r) mm, in 4 mm cells.
  A new mark fits when every one of its discs keeps `gap` mm from every disc
  already placed."""

  CELL = 4.0

  def __init__(self):
    self.cells = {}

  def _keys(self, u, v, reach):
    c = self.CELL
    for i in range(int((u - reach) // c), int((u + reach) // c) + 1):
      for j in range(int((v - reach) // c), int((v + reach) // c) + 1):
        yield i, j

  def fits(self, discs, gap=GAP):
    for u, v, r in discs:
      for key in self._keys(u, v, r + gap):
        for pu, pv, pr in self.cells.get(key, ()):
          if (pu - u) ** 2 + (pv - v) ** 2 < (pr + r + gap) ** 2:
            return False
    return True

  def add(self, discs):
    for disc_ in discs:
      for key in set(self._keys(disc_[0], disc_[1], disc_[2])):
        self.cells.setdefault(key, []).append(disc_)


class Sky:
  """The light mask, drawn one mark at a time inside a window round each mark."""

  def __init__(self):
    self.light = np.zeros((H, W), bool)

  @staticmethod
  def window(u0, v0, u1, v1):
    i0, i1 = max(0, int(u0 * S)), min(W, int(u1 * S) + 2)
    j0, j1 = max(0, int(v0 * S)), min(H, int(v1 * S) + 2)
    if i0 >= i1 or j0 >= j1:
      return None
    return slice(j0, j1), slice(i0, i1)

  def taper(self, u0, v0, u1, v1, r0, r1, value=True):
    """A capsule from (u0, v0) to (u1, v1), its radius running r0 to r1 mm."""
    rr = max(r0, r1)
    win = self.window(min(u0, u1) - rr, min(v0, v1) - rr, max(u0, u1) + rr, max(v0, v1) + rr)
    if win is None:
      return
    uu, vv = U[win], V[win]
    du, dv = u1 - u0, v1 - v0
    l2 = du * du + dv * dv
    t = np.clip(((uu - u0) * du + (vv - v0) * dv) / l2, 0, 1) if l2 > 0 else 0.0
    inside = np.hypot(uu - u0 - t * du, vv - v0 - t * dv) < r0 + t * (r1 - r0)
    if value:
      self.light[win] |= inside
    else:
      self.light[win] &= ~inside

  def dot(self, u, v, d, value=True):
    self.taper(u, v, u, v, d / 2, d / 2, value)


def smooth01(x):
  x = np.clip(x, 0, 1)
  return x * x * (3 - 2 * x)


def blobs(rng, n, lo, hi):
  """A smooth field in 0..1: n round blobs lo to hi mm across (one sigma),
  summed and scaled so the richest 3 % of the face reads 1."""
  f = np.zeros(U.shape)
  for _ in range(n):
    cu, cv = rng.uniform(-20, W / S + 20), rng.uniform(-20, H / S + 20)
    sg = rng.uniform(lo, hi)
    f += rng.uniform(0.4, 1.0) * np.exp(-((U - cu) ** 2 + (V - cv) ** 2) / (2 * sg * sg))
  return np.clip(f / np.percentile(f, 97), 0, 1)


# --- Stars --------------------------------------------------------------------

def magnitudes(n, rng, faint=6.0, steep=0.8):
  """n magnitudes, 0 the brightest, `faint` the faintest, sorted brightest
  first. Fainter stars are commoner, by e**steep per magnitude, like a real
  sky's star counts."""
  x = rng.random(n)
  return np.sort(np.log1p(x * np.expm1(steep * faint)) / steep)


def spike_segments(u, v, length, angle, w0, n=4):
  """n spikes from (u, v), each `length` mm: w0 mm wide at the centre,
  narrowing to MIN_W at 40 % of the length, then MIN_W to a round tip."""
  segs = []
  for k in range(n):
    a = angle + k * 2 * math.pi / n
    ca, sa = math.cos(a), math.sin(a)
    ku, kv = u + 0.4 * length * ca, v + 0.4 * length * sa
    segs.append((u, v, ku, kv, w0 / 2, MIN_W / 2))
    segs.append((ku, kv, u + length * ca, v + length * sa, MIN_W / 2, MIN_W / 2))
  return segs


def cover(segs):
  """Discs every 0.5 mm along each tapered segment, for the Room."""
  out = []
  for u0, v0, u1, v1, r0, r1 in segs:
    n = max(1, int(math.hypot(u1 - u0, v1 - v0) / 0.5))
    out += [(u0 + (u1 - u0) * k / n, v0 + (v1 - v0) * k / n, r0 + (r1 - r0) * k / n)
            for k in range(n + 1)]
  return out


def star(sky, room, u, v, d, spike=0.0, angle=0.0, sparkle=0.0, gap=GAP, blocked=None):
  """A star d mm across at (u, v). With spike > 0 it gets four spikes that
  reach that far from its centre; sparkle > 0 adds four shorter spikes
  between them, that long. Drawn only if it fits; returns whether it did."""
  segs = []
  if spike > d / 2:
    segs += spike_segments(u, v, spike, angle, max(0.55 * d, MIN_W))
  if sparkle > d / 2:
    segs += spike_segments(u, v, sparkle, angle + math.pi / 4, max(0.4 * d, MIN_W))
  discs = [(u, v, d / 2)] + cover(segs)
  if not fits_face(discs, blocked) or not room.fits(discs, gap):
    return False
  room.add(discs)
  sky.dot(u, v, d)
  for seg in segs:
    sky.taper(*seg)
  return True


def scatter(sky, room, mags, size, rng, where=None, spike=None, angle=0.0, sparkle=None,
            gap=GAP, blocked=None, tries=60, region=None):
  """Places one star per magnitude, brightest first, each at a random spot
  accepted with probability where(u, v) (a raster in 0..1, or everywhere).
  size(m) is the dot size, mm; spike(m) and sparkle(m) the spike lengths."""
  u0, v0, u1, v1 = region or (0, 0, W / S, H / S)
  placed = 0
  for m in mags:
    d = size(m)
    for _ in range(tries):
      u, v = rng.uniform(u0, u1), rng.uniform(v0, v1)
      if where is not None:
        p = at(where, u, v)
        if p is None or rng.random() >= p:
          continue
      if star(sky, room, u, v, d, spike(m) if spike else 0.0, angle,
              sparkle(m) if sparkle else 0.0, gap, blocked):
        placed += 1
        break
  return placed


def stipple(sky, room, tone, rng, darts, d_lo=MIN_W, d_hi=1.25, s_hi=9.0, blocked=None,
            region=None):
  """Light dots thrown at random inside `region` (u0, v0, u1, v1 mm, or the
  whole face) and kept where they fit. Their spacing falls from s_hi mm
  where tone is near 0 to the closest printable spacing where tone is 1, so
  the share of light follows tone; the dots grow from d_lo to d_hi mm with it."""
  u0, v0, u1, v1 = region or (0, 0, W / S, H / S)
  us, vs = rng.uniform(u0, u1, darts), rng.uniform(v0, v1, darts)
  ii, jj = np.clip((us * S).astype(int), 0, W - 1), np.clip((vs * S).astype(int), 0, H - 1)
  t = tone[jj, ii]
  keep = t > 0.01
  if blocked is not None:
    keep &= ~blocked[jj, ii]
  d = d_lo + (d_hi - d_lo) * t
  s = np.minimum(s_hi, (d + GAP) / np.sqrt(np.maximum(t, 1e-6)))
  keep &= FREE[jj, ii] > d / 2 + 0.15

  # Brighter darts go first, in eight bands of tone, at random within a
  # band: a sparse dot thrown early would otherwise block a dense run of
  # dots beside it.

  keep = np.nonzero(keep)[0]
  keep = keep[np.lexsort((rng.random(keep.size), -np.floor(t[keep] * 8)))]
  for u, v, dd, ss in zip(us[keep], vs[keep], d[keep], s[keep]):
    discs = [(u, v, dd / 2)]
    if room.fits(discs, ss - dd):
      room.add(discs)
      sky.dot(u, v, dd)


def speckle(sky, solid, tone, rng, darts, d_dark=0.8, s_hi=6.0):
  """The brightest tones drawn the other way round: solid light with dark
  specks d_dark mm across, sparser as the tone rises, each speck at least
  MIN_W of light from the next and from the solid's edge."""
  inner = ~dilate(~solid, disc(int(2 * (d_dark / 2 + MIN_W) * S)))
  sky.light |= solid
  us, vs = rng.uniform(0, W / S, darts), rng.uniform(0, H / S, darts)
  ii, jj = np.clip((us * S).astype(int), 0, W - 1), np.clip((vs * S).astype(int), 0, H - 1)
  keep = inner[jj, ii]
  dark = 1 - tone[jj, ii]
  s = np.minimum(s_hi, (d_dark + MIN_W) / np.sqrt(np.maximum(dark, 1e-6)))
  room = Room()
  for u, v, ss in zip(us[keep], vs[keep], s[keep]):
    discs = [(u, v, d_dark / 2)]
    if room.fits(discs, ss - d_dark):
      room.add(discs)
      sky.dot(u, v, d_dark, value=False)


def lit(h, toward):
  """Cosine between the surface normal of height field h (mm) and the light
  coming from `toward` (u, v, z), v pointing down the face."""
  lu, lv, lz = np.array(toward) / np.linalg.norm(toward)
  hv, hu = np.gradient(h, 1 / S)
  return np.clip((lz - lu * hu - lv * hv) / np.sqrt(1 + hu * hu + hv * hv), 0, 1)


def column_curve(points, smooth):
  """A curve v(u) through `points` (u, v) mm, one value per pixel column,
  rounded by a box blur `smooth` mm wide; and its slope."""
  cols = (np.arange(W) + 0.5) / S
  xs, ys = zip(*sorted(points))
  c = np.interp(cols, xs, ys)
  n = int(smooth * S / 2)
  for _ in range(3):
    c = np.convolve(np.pad(c, n, mode='edge'), np.ones(2 * n + 1) / (2 * n + 1), 'valid')
  return c, np.gradient(c, 1 / S)


# --- H0: a sparse sky of mixed magnitudes ------------------------------------------

def h0_sparse():
  """About 1200 stars, magnitudes 0 to 6. Dot size follows brightness,
  1.0 mm for the faintest up to 3.8 mm. The faint stars gather in loose
  drifts and thin out between them; the bright ones fall anywhere. The
  brightest 1 % carry four spikes 6 to 14 mm long, the brightest three
  four short diagonal spikes as well, so they sparkle."""
  rng = np.random.default_rng(3)
  sky, room = Sky(), Room()
  drift = 0.15 + 0.85 * blobs(rng, 34, 7, 26)
  mags = magnitudes(1200, rng)

  def size(m):
    return MIN_W + 2.8 * ((6 - m) / 6) ** 2.2

  def spike(m):
    return 0.0 if m > 0.9 else 2.2 * size(m) + 6.0 * (0.9 - m)

  def sparkle(m):
    return 0.0 if m > mags[2] else 0.45 * spike(m)

  bright, faint = mags[mags < 3], mags[mags >= 3]
  scatter(sky, room, bright, size, rng, spike=spike, sparkle=sparkle, gap=1.2)
  scatter(sky, room, faint, size, rng, where=drift, gap=1.2)
  return sky.light


# --- H1: a dense field with the Milky Way ---------------------------------------------

def h1_milky_way():
  """A dense field crossed by the Milky Way, rising from lower left to upper
  right, wider and brighter toward the lower left where the galactic centre
  lies off the face. Its star clouds are stipple that thickens toward the
  band's spine, the dots growing as they crowd. A dark dust rift runs along
  the band and dark filaments branch off it."""
  rng = np.random.default_rng(11)
  sky, room = Sky(), Room()
  slope = 0.56
  centre = 118 - slope * U + 6 * np.sin(U / 37 + 0.5) + 3 * wave_noise(U, V, 50, 90)
  n = (V - centre) / math.sqrt(1 + slope * slope)
  toward = np.clip((170 - U) / 170, 0, 1)
  sig = 10 + 10 * toward
  band = np.exp(-0.5 * (n / sig) ** 2)
  clouds = smooth01(0.6 + 0.3 * wave_noise(U, V, 51, 30) + 0.2 * wave_noise(U, V, 52, 12))
  glow = 0.55 + 0.45 * toward

  # The rift wanders along the band; filaments are the ridges of a noise field.

  rift_off = -1.0 + 4.0 * wave_noise(U, V, 53, 70)
  rift_w = (0.8 + 3.2 * toward) * (0.75 + 0.25 * wave_noise(U, V, 54, 23))
  rift = 1 - smooth01((np.abs(n - rift_off) - rift_w) / 1.5)
  ridge = 1 - np.abs(wave_noise(U, V, 55, 22) + 0.4 * wave_noise(U, V, 56, 9))
  fil = smooth01((ridge - 0.86) / 0.08) * smooth01((band - 0.25) / 0.3)
  dust = np.maximum(rift, fil)

  tone = blur(band ** 0.8 * (0.4 + 0.6 * clouds) * glow * (1 - dust), 0.6)
  tone = np.clip(tone / np.percentile(tone, 98), 0, 1)

  bright = magnitudes(110, rng, faint=4.0)
  scatter(sky, room, bright, lambda m: MIN_W + 1.6 * ((4 - m) / 4) ** 2, rng, gap=1.0)
  stipple(sky, room, 0.09 + 0.91 * tone ** 1.2, rng, 1200000, d_hi=1.4, s_hi=7.0)
  return sky.light


# --- H2: star clusters and a nebula wisp --------------------------------------------------

H2_STREAM = [(-20.0, 79.0), (10.0, 75.0), (40.0, 71.0), (70.0, 69.0), (100.0, 65.0),
             (126.0, 53.0), (146.0, 32.0), (162.0, 14.0)]


def h2_clusters():
  """An open cluster of spiked stars at the upper right, a globular cluster at
  the upper left, and a reflection nebula streaming from the open cluster
  down and to the left through the clear band above the knobs, a second
  strand splitting off above it. The nebula is a billowing cloud lit from
  the upper right, by the cluster, drawn in flow lines along the stream that
  swell on the lit side of each billow and thin away into its shade."""
  rng = np.random.default_rng(23)
  sky, room = Sky(), Room()

  # The stream: flow lines are level sets of psi, the height below the
  # stream's spine; n is the true distance across it.

  spine, slope = column_curve(H2_STREAM, 14.0)
  psi = V - spine[None, :]
  n = psi / np.sqrt(1 + slope[None, :] ** 2)
  along = (np.cumsum(np.sqrt(1 + slope ** 2)) / S)[None, :]
  width = 2.5 + 5.0 * smooth01((150 - U) / 100)
  start = smooth01((160 - U) / 16)
  wander = 1.5 * wave_noise(U / 3, V * 0, 71, 12)
  main = np.exp(-0.5 * ((n - wander) / width) ** 2)
  split = -10.0 * smooth01((122 - U) / 80)
  branch = 0.75 * np.exp(-0.5 * ((n - split) / (0.55 * width)) ** 2) * smooth01((124 - U) / 25)
  env = np.maximum(main, branch) * start * (1 - 0.7 * smooth01((80 - U) / 80))
  billow = 0.5 + 0.3 * wave_noise(U / 3, n, 73, 7) + 0.2 * wave_noise(U / 2, n, 74, 3.5)
  h = 3.0 * env * np.clip(billow, 0.1, None)
  cloud = env > 0.1
  tone = equalize(lit(h, (1.0, -1.0, 1.1)), cloud)
  tone = blur(tone, 0.3) * smooth01((env - 0.1) / 0.5)

  # Flow lines 2.3 mm apart: solid where the cloud is bright, swelling to
  # the spacing less GAP; where it fades, dashes 3 mm apart that shorten to
  # dots. True line spacing comes from the gradient of the warped coordinate.

  flow = psi + 0.6 * wave_noise(U / 3, V, 75, 12) + 0.3 * wave_noise(U, V, 76, 16)
  gv, gu = np.gradient(flow, 1 / S)
  stretch = np.hypot(gu, gv)
  period = 2.3 / stretch
  dist = line_dist(flow, 2.3) / stretch
  k = np.clip((tone - 0.5) / 0.45, 0, 1)
  w = MIN_W + k * np.maximum(period - GAP - MIN_W, 0)
  stagger = 3.0 * hash01(np.round(flow / 2.3).astype(np.int64), 77)
  duty = 0.34 + 0.39 * np.clip((tone - 0.18) / 0.32, 0, 1)
  dash = np.mod(along + stagger, 3.0) < 3.0 * duty
  nebula = (dist < w / 2) & ((tone >= 0.5) | ((tone >= 0.18) & dash)) & (FREE > 0)
  sky.light |= nebula
  blocked = dilate(nebula, disc(int(2 * GAP * S) + 2))

  def size(m):
    return MIN_W + 2.6 * ((6 - m) / 6) ** 2.2

  def spike(m):
    return 0.0 if m > 1.2 else 2.0 * size(m) + 5.0 * (1.2 - m)

  def field_spike(m):
    return 0.0 if m > 0.5 else spike(m)

  # The open cluster: seven bright spiked stars and their fainter kin.

  cu, cv = 152.0, 20.0
  near = np.exp(-0.5 * ((U - cu) ** 2 + (V - cv) ** 2) / 9.0 ** 2)
  box = (cu - 24, cv - 24, cu + 24, cv + 24)
  scatter(sky, room, magnitudes(7, rng, faint=1.2, steep=0.3), size, rng, where=near,
          spike=spike, gap=1.0, blocked=blocked, tries=600, region=box)
  scatter(sky, room, magnitudes(30, rng, faint=4.5, steep=0.4) + 1.5, size, rng, where=near,
          gap=1.0, blocked=blocked, tries=600, region=box)

  # The globular cluster: a King profile in stipple, the dots crowding and
  # growing toward the core.

  gu_, gv_ = 33.0, 19.0
  r = np.hypot(U - gu_, V - gv_)
  rc, rt = 3.5, 21.0
  king = np.clip(1 / np.sqrt(1 + (r / rc) ** 2) - 1 / math.sqrt(1 + (rt / rc) ** 2), 0, None)
  king = np.where(r < rt, np.clip(1.2 * (king / king.max()) ** 0.7, 0, 1), 0.0)
  stipple(sky, room, king, rng, 90000, d_hi=1.4, s_hi=8.0,
          region=(gu_ - rt, gv_ - rt, gu_ + rt, gv_ + rt))

  # The field: sparse background stars, the brightest few spiked.

  scatter(sky, room, magnitudes(650, rng), size, rng, spike=field_spike, gap=1.1,
          blocked=blocked)
  return sky.light


# --- H3: star trails ------------------------------------------------------------------

H3_POLE = (149.0, 29.0)


def h3_trails():
  """A long exposure: every star draws an arc of 42 degrees about the pole,
  which sits at the upper right. Line width follows brightness, 1.0 to
  2.4 mm; each trail tapers from its full width at the head, where the
  exposure ended, to 1.0 mm at the tail, so the sky turns counter-clockwise.
  Polaris draws a short bright stub beside the pole."""
  rng = np.random.default_rng(31)
  sky = Sky()
  pu, pv = H3_POLE
  span = math.radians(42)
  rad = np.hypot(U - pu, V - pv).ravel()
  ang = np.arctan2(V - pv, U - pu).ravel()
  order = np.argsort(rad)
  rad_sorted = rad[order]
  flat = sky.light.ravel()
  placed = {}

  def fits(r, a, w):
    for key in range(int((r - 3) // 2), int((r + 3) // 2) + 1):
      for r2, a2, w2 in placed.get(key, ()):
        if abs(r - r2) < (w + w2) / 2 + GAP:
          pad = (max(w, w2) / 2 + GAP) / min(r, r2)
          da = abs((a - a2 + math.pi) % (2 * math.pi) - math.pi)
          if da < span + pad:
            return False
    return True

  def draw(r, a, w):
    """The arc runs from its head at angle a - span / 2 (counter-clockwise,
    v down) to its tail at a + span / 2."""
    head = a - span / 2
    lo, hi = np.searchsorted(rad_sorted, [r - w / 2 - 0.1, r + w / 2 + 0.1])
    idx = order[lo:hi]
    f = ((ang[idx] - head) % (2 * math.pi)) / span
    half = (w + (MIN_W - w) * np.clip(f, 0, 1)) / 2
    flat[idx[(f <= 1) & (np.abs(rad[idx] - r) < half)]] = True
    sky.dot(pu + r * math.cos(head), pv + r * math.sin(head), w)
    tail = head + span
    sky.dot(pu + r * math.cos(tail), pv + r * math.sin(tail), MIN_W)
    placed.setdefault(int(r // 2), []).append((r, a, w))

  # Polaris, 3 mm from the pole, draws first.

  draw(3.2, math.radians(200), 2.3)

  def on_face(r, a):
    for f in np.linspace(-0.5, 0.5, 9):
      u, v = pu + r * math.cos(a + f * span), pv + r * math.sin(a + f * span)
      if (at(FREE, u, v) or -1) > 0:
        return True
    return False

  mags = magnitudes(2600, rng)
  for m in mags:
    w = MIN_W + 1.4 * ((6 - m) / 6) ** 2.2
    for _ in range(25):
      r = 205 * math.sqrt(rng.random())
      if r < 6:
        continue
      a = rng.uniform(-math.pi, math.pi)
      if on_face(r, a) and fits(r, a, w):
        draw(r, a, w)
        break
  return sky.light


# --- H4: a deep field of faint galaxies --------------------------------------------------

def galaxy(tone, solid, u, v, a, q, phi, kind, rng):
  """One galaxy at (u, v): major semi-axis a mm, axis ratio q, major axis at
  angle phi. Its brightness goes into `tone`, which the stipple follows, and
  its bright core into `solid`. A `smudge` is all core, a small solid oval;
  an `elliptical` fades smoothly from its core; a `spiral`, seen tilted,
  carries its light in two arms winding out from the core; an `edge-on`
  disc is a thin streak of core in a narrow haze."""
  win = Sky.window(u - a - 1, v - a - 1, u + a + 1, v + a + 1)
  if win is None:
    return
  du, dv = U[win] - u, V[win] - v
  c, s = math.cos(phi), math.sin(phi)
  x = du * c + dv * s
  across = -du * s + dv * c
  r = np.hypot(x, across / q)

  # A core is at least 1.2 mm across its narrow way.

  if kind == 'smudge':
    b = max(q * a, 0.6)
    solid[win] |= (x / a) ** 2 + (across / b) ** 2 < 1
    return
  if kind == 'edge-on':
    t = np.clip(1 - np.abs(x) / a, 0, 1) ** 0.7 * np.exp(-0.5 * (across / max(q * a, 0.6)) ** 2)
    core = (np.abs(across) < 0.6) & (np.abs(x) < 0.45 * a)
  elif kind == 'elliptical':
    t = np.clip(1 - r / a, 0, 1) ** 1.3
    core = r < max(0.2 * a, 0.6 / q)
  else:
    pitch = math.radians(rng.uniform(14, 20))
    rc = max(0.14 * a, 0.6 / q)
    wind = np.arctan2(across / q, x) - np.log(np.maximum(r, 0.05) / rc) / math.tan(pitch)
    arm = 0.5 + 0.5 * np.cos(2 * wind + rng.uniform(0, 2 * math.pi))
    t = np.clip(1 - r / a, 0, 1) ** 0.4 * arm ** 2
    core = r < rc
  tone[win] = np.maximum(tone[win], t)
  solid[win] |= core


def h4_deep_field():
  """A deep field: about 700 galaxies at every distance, a few big near ones
  and ever more, ever smaller ones behind them, down to 1.2 mm smudges. Each
  big galaxy is a solid core in stipple that follows its light: spirals
  show two dotted arms, ellipticals a halo that thins outward, edge-on discs
  a narrow streak. Six foreground stars carry four-point spikes."""
  rng = np.random.default_rng(41)
  sky, room = Sky(), Room()
  tone, solid = np.zeros(U.shape), np.zeros(U.shape, bool)

  stars = magnitudes(6, rng, faint=2.0, steep=0.3)
  scatter(sky, room, stars, lambda m: MIN_W + 1.4 * ((2.5 - m) / 2.5), rng,
          spike=lambda m: 5.0 + 3.0 * (2.0 - m), gap=1.2)

  # Sizes follow a power law; the biggest galaxies place first. The Room
  # holds each galaxy's outline only while placing; the stipple keeps its
  # own Room.

  shapes = Room()
  for disc_ in room.cells.values():
    for d_ in disc_:
      shapes.add([d_])
  hero = (92.0, 74.0, 13.0, 0.6, math.radians(-28))
  shapes.add([(hero[0], hero[1], hero[2])])
  galaxy(tone, solid, *hero, 'spiral', rng)
  sizes = np.sort(np.clip(0.6 * (1 - rng.random(700)) ** (-1 / 1.3), 0.6, 9.5))[::-1]
  for a in sizes:
    q = rng.uniform(0.4, 0.95)
    if a < 2.2:
      kind, q = 'smudge', rng.uniform(0.45, 0.9)
    elif a < 5.0:
      kind = rng.choice(['elliptical', 'edge-on', 'spiral'], p=[0.6, 0.2, 0.2])
    else:
      kind = rng.choice(['spiral', 'elliptical', 'edge-on'], p=[0.55, 0.25, 0.2])
    if kind == 'edge-on':
      q = rng.uniform(0.15, 0.25)
    phi = rng.uniform(0, math.pi)
    for _ in range(80):
      u, v = rng.uniform(0, W / S), rng.uniform(0, H / S)
      discs = [(u, v, a + 0.2)]
      if fits_face(discs) and shapes.fits(discs, GAP + 0.6):
        shapes.add(discs)
        galaxy(tone, solid, u, v, a, q, phi, kind, rng)
        break

  solid &= FREE > 0.15
  sky.light |= solid
  blocked = dilate(solid, disc(int(2 * (GAP + 0.6) * S)))
  stipple(sky, room, tone, rng, 1500000, d_hi=1.2, s_hi=5.0, blocked=blocked)
  return sky.light


# --- Output -------------------------------------------------------------------

CANDIDATES = [
  ('H0', h0_sparse, 'tbd', 'tbd'),
  ('H1', h1_milky_way, 'tbd', 'tbd'),
  ('H2', h2_clusters, 'tbd', 'tbd'),
  ('H3', h3_trails, 'tbd', 'tbd'),
  ('H4', h4_deep_field, 'tbd', 'tbd'),
]


def render(name, fn):
  t0 = time.time()
  light, changed = printable(fn())
  img = face_image(light)
  img.save(OUT / f'{name}.png')
  print(f'{name}: {changed * 100:.2f} % of light pixels changed, {time.time() - t0:.1f} s')
  return img, changed


def main(names):
  OUT.mkdir(parents=True, exist_ok=True)
  chosen = [c for c in CANDIDATES if not names or c[0] in names]
  results, lines = [], []
  for name, fn, what, how in chosen:
    img, changed = render(name, fn)
    results.append((name, img))
    lines.append(f'{name}: {what}. Highlight and shadow: {how}. '
                 f'Printability pass changed {changed * 100:.2f} % of light pixels.')
  print('\n'.join(lines))


if __name__ == '__main__':
  main(sys.argv[1:])
