"""The whole plan from a source and a face to the pieces to cut."""

import time
from dataclasses import dataclass, field

from . import clip, raster, trace
from .errors import DecalError
from .raster import PITCH_MM

CEILING_POINTS = 10_000
CEILING_PIECES = 1_000
# Measured 2026-11-04 in Fusion, 6,802 points in 78 s (sketch, profile match, cut, stored body).
POINTS_PER_MINUTE = 5_000

# Measured 2026-11-04, one emboss per piece at 0.29 s plus 0.24 s to delete it.
CYLINDER_SECONDS_PER_PIECE = 0.55

SIMPLIFY_MM = 0.08

DEFAULTS = dict(mode='dark', threshold=128, invert=False, width_mm=None, offset_x=0.0,
                offset_y=0.0, rotation=0, mirror=False, min_width_mm=0.8, min_area_mm2=0.5,
                keepouts=True, hole_keepouts={}, margin_mm=0.2, depth_mm=0.4, store_cut=True)


@dataclass
class Face2D:
    """The target face flattened to millimetres, x right and y up.

    kind is 'flat' or 'cylinder'. outer and inners are closed loops, holes are round holes
    as (x, y, diameter) and are not repeated in inners. centre is the clicked point.
    width_mm is the bounding-box width for a flat face and the arc length for a cylinder.
    """

    kind: str = 'flat'
    outer: list = field(default_factory=list)
    inners: list = field(default_factory=list)
    holes: list = field(default_factory=list)
    centre: tuple = (0.0, 0.0)
    width_mm: float = 0.0


@dataclass
class Source:
    """Art as an 8-bit image (kind 'image') or as closed loops with y up (kind 'loops')."""

    kind: str
    w: int = 0
    h: int = 0
    grey: bytearray = None
    alpha: bytearray = None
    loops: list = field(default_factory=list)


@dataclass
class Result:
    """What compute decides. pieces are in face coordinates, outer loops counter-clockwise."""

    pieces: list
    points: int
    pieces_count: int
    dropped_thin: int
    dropped_small: int
    area_mm2: float
    est_minutes: float
    over: str = None
    warnings: list = field(default_factory=list)
    timings: dict = field(default_factory=dict)


def check_ceilings(points: int, pieces: int):
    """Return the reason text when a count passes its ceiling, else None."""
    reasons = []
    if points > CEILING_POINTS:
        reasons.append(f"The art has {points:,} points and the ceiling is {CEILING_POINTS:,}. "
                       "Raise the minimum width or use simpler art.")
    if pieces > CEILING_PIECES:
        reasons.append(f"The art has {pieces:,} pieces and the ceiling is {CEILING_PIECES:,}. "
                       "Raise the minimum area or use simpler art.")
    return " ".join(reasons) or None


def _keepout_diameter(d: float, table: dict, margin: float) -> float:
    key = round(d, 1)
    for k in (key, str(key)):
        if k in table:
            return float(table[k])
    return d + 2 * margin


def _mask_from_source(source: Source, width_mm: float, cfg: dict, tick=None) -> raster.Mask:
    if source.kind == 'image':
        return raster.from_grey(source.w, source.h, source.grey, source.alpha, width_mm,
                                cfg['mode'], cfg['threshold'], cfg['invert'], tick)
    if source.kind == 'loops':
        mask = raster.from_loops(source.loops, width_mm)
        if cfg['invert']:
            mask.bits = mask.bits.translate(raster._FLIP01)
        return mask
    raise DecalError(f"The source kind {source.kind!r} is unknown. Use an image or loops.")


# Share of the whole run each stage takes, in order. The shares sum to 1.
STAGE_WEIGHTS = (('mask', 0.45), ('orient', 0.03), ('keepouts', 0.07), ('width', 0.10),
                 ('trace', 0.30), ('map', 0.05))

# A loop reports to the callback when it has advanced this fraction of its own stage.
REPORT_STEP = 0.02


class _Reporter:
    """Turns stage boundaries and loop ticks into progress(stage, fraction) calls.

    Fractions never decrease. An exception raised by the callback propagates.
    """

    def __init__(self, progress):
        self.progress = progress
        self.sent = 0.0
        self.stage = STAGE_WEIGHTS[0][0]
        self.lo = 0.0
        self.hi = 0.0
        self.last = 0.0
        self.starts = {}
        done = 0.0
        for name, weight in STAGE_WEIGHTS:
            self.starts[name] = (done, min(1.0, done + weight))
            done += weight

    def emit(self, value):
        self.sent = min(1.0, max(self.sent, value))
        self.progress(self.stage, self.sent)

    def begin(self, name):
        self.stage = name
        self.lo, self.hi = self.starts[name]
        self.last = 0.0
        if name == STAGE_WEIGHTS[0][0]:
            self.emit(0.0)

    def end(self):
        self.emit(self.hi if self.stage != STAGE_WEIGHTS[-1][0] else 1.0)

    def tick(self, f):
        if f >= 1.0 or f - self.last >= REPORT_STEP:
            self.last = f
            self.emit(self.lo + (self.hi - self.lo) * f)


def compute(source: Source, face: Face2D, settings: dict, progress=None) -> Result:
    """Run the pipeline and return the pieces with their counts.

    Raises DecalError when the art is wider than the face or nothing is left. A ceiling
    that is passed does not raise. It sets Result.over so the dialog can show the reason.

    progress, when given, is called as progress(stage, fraction) at every stage boundary
    and about every 2 % of each slow loop. The fraction covers the whole run, 0 to 1, and
    never decreases. The callback may raise to cancel, and the exception propagates.
    """
    cfg = dict(DEFAULTS)
    cfg.update(settings)
    margin = cfg['margin_mm']
    timings = {}
    reporter = _Reporter(progress) if progress else None
    tick = reporter.tick if reporter else None
    clock = time.perf_counter()

    def begin(name):
        if reporter:
            reporter.begin(name)

    def lap(name):
        nonlocal clock
        now = time.perf_counter()
        timings[name] = now - clock
        clock = now
        if reporter:
            reporter.end()

    width = cfg['width_mm'] or (face.width_mm - 2 * margin)
    if width <= 0:
        raise DecalError(f"The face is {face.width_mm:.1f} mm wide, which leaves no room for art "
                         f"inside a margin of {margin:g} mm.")
    degrees = cfg['rotation']
    if degrees % 90:
        raise DecalError(f"The rotation {degrees} is not 0, 90, 180 or 270 degrees.")

    begin('mask')
    mask = _mask_from_source(source, width, cfg, tick)
    lap('mask')
    warnings = list(mask.warnings)

    begin('orient')
    mask = raster.rotate(mask, degrees // 90)
    if cfg['mirror']:
        mask = raster.mirror(mask)
    art_w = mask.w * PITCH_MM
    art_h = mask.h * PITCH_MM
    if face.kind == 'cylinder':
        limit = face.width_mm - 2 * margin
        what = "the face's arc length less two margins"
    else:
        limit = face.width_mm
        what = "the face"
    if art_w > limit + PITCH_MM:
        raise DecalError(f"The art is {art_w:.1f} mm wide and {what} allows {limit:.1f} mm. "
                         "Reduce the width or the margin.")
    lap('orient')

    begin('keepouts')
    cx, cy = face.centre
    ox = cx + cfg['offset_x']
    oy = cy + cfg['offset_y']

    def to_frame(loop):
        return [(x - ox + art_w / 2, oy + art_h / 2 - y) for x, y in loop]

    before = mask
    outer = to_frame(face.outer)
    inners = [to_frame(loop) for loop in face.inners]
    holes = [(x - ox + art_w / 2, oy + art_h / 2 - y, d) for x, y, d in face.holes]
    if cfg['keepouts']:
        room = clip.allowed(mask.w, mask.h, outer, inners, margin)
        circles = [(u, v, _keepout_diameter(d, cfg['hole_keepouts'], margin))
                   for (u, v, _), (_, _, d) in zip(holes, face.holes)]
    else:
        room = clip.allowed(mask.w, mask.h, outer, inners, 0.0)
        circles = [(u, v, d) for u, v, d in holes]
    mask = clip.clear_circles(raster.intersect(mask, room), circles)
    lap('keepouts')

    begin('width')
    mask, dropped_thin = raster.open_close(mask, cfg['min_width_mm'], tick)
    lap('width')

    begin('trace')
    pieces, dropped_small = trace.loops(mask, SIMPLIFY_MM, cfg['min_area_mm2'], tick)
    lap('trace')
    if not pieces:
        raise DecalError(
            f"Nothing is left of the art after keep-outs and minimum width. It had "
            f"{raster.count_pieces(before)} pieces before and has none after, with "
            f"{dropped_thin} lost to the minimum width and {dropped_small} to the minimum area.")

    begin('map')

    def to_face(loop):
        return [(ox + (u - art_w / 2), oy - (v - art_h / 2)) for u, v in loop]

    placed = [trace.Piece(to_face(p.outer), [to_face(h) for h in p.holes], p.area_mm2)
              for p in pieces]
    count = trace.points(placed)
    lap('map')
    return Result(
        pieces=placed,
        points=count,
        pieces_count=len(placed),
        dropped_thin=dropped_thin,
        dropped_small=dropped_small,
        area_mm2=sum(p.area_mm2 for p in placed),
        est_minutes=count / POINTS_PER_MINUTE + (
            len(placed) * CYLINDER_SECONDS_PER_PIECE / 60 if face.kind == 'cylinder' else 0.0),
        over=check_ceilings(count, len(placed)),
        warnings=warnings,
        timings=timings,
    )
