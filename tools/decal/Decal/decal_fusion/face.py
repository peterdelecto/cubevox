"""The picked face as decal_core sees it, and the frame that maps it back to the model.

Face coordinates are millimetres with y up, origin at the clicked point. On a flat face x and
y lie in the plane. On a cylinder x is arc length round the axis and y runs along the axis.
Model points are centimetres in the space of the face's own component; Frame.world() takes
them to the root for custom graphics.

The frame's axes come from the component's axes, never from the face's own parameter
directions, so the same plane or cylinder always gives the same frame. A rebake re-finds the
face on a copied body, and a copy may carry different parameter directions.
"""

import math

import adsk.core
import adsk.fusion

from decal_core.errors import DecalError
from decal_core.plan import Face2D

Vec = tuple[float, float, float]

STROKE_CM = 0.002          # chord tolerance of the face loops, 0.02 mm
PROBE_CM = 0.001           # step off the face for the inside or outside test, 0.01 mm
SAME_CM = 1e-4             # points, centres and radii closer than this coincide
PARALLEL = 0.99999         # |cos| above this counts as parallel
POCKET_STEP_MM = 0.05      # probe inset from a loop edge; decal pieces are at least 0.8 mm wide
POCKET_DEPTH_MM = 0.1      # probe depth under the face; inlays are at least this deep

_SURFACE_NAMES = {
    adsk.core.SurfaceTypes.ConeSurfaceType: 'a cone',
    adsk.core.SurfaceTypes.SphereSurfaceType: 'a sphere',
    adsk.core.SurfaceTypes.TorusSurfaceType: 'a torus',
    adsk.core.SurfaceTypes.EllipticalCylinderSurfaceType: 'an elliptical cylinder',
    adsk.core.SurfaceTypes.EllipticalConeSurfaceType: 'an elliptical cone',
    adsk.core.SurfaceTypes.NurbsSurfaceType: 'a free-form (NURBS) surface',
}


# --- Vectors as tuples -----------------------------------------------------------------

def _sub(a: Vec, b: Vec) -> Vec:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _add(a: Vec, b: Vec) -> Vec:
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def _mul(a: Vec, s: float) -> Vec:
    return (a[0] * s, a[1] * s, a[2] * s)


def _dot(a: Vec, b: Vec) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a: Vec, b: Vec) -> Vec:
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _norm(a: Vec) -> float:
    return math.sqrt(_dot(a, a))


def _unit(a: Vec) -> Vec:
    n = _norm(a)
    if n < 1e-12:
        raise ValueError(f'zero-length vector {a}')
    return (a[0] / n, a[1] / n, a[2] / n)


def xyz(p) -> Vec:
    """A Fusion Point3D or Vector3D as a tuple."""
    return (p.x, p.y, p.z)


def point(v: Vec) -> 'adsk.core.Point3D':
    return adsk.core.Point3D.create(v[0], v[1], v[2])


def _wrap(a: float) -> float:
    """An angle folded into (-pi, pi]."""
    a = math.fmod(a + math.pi, 2 * math.pi)
    if a <= 0:
        a += 2 * math.pi
    return a - math.pi


# --- The frame -------------------------------------------------------------------------

class Frame:
    """Maps face coordinates (mm) to model space (cm) for one face.

    origin is the clicked point on the face. ex and ey are the face's x and y directions at
    the click and n is the outward normal there. On a cylinder, centre is the axis point
    level with the click, radial the unit vector from it to the click, and sign is +1 when
    the face looks away from the axis and -1 when it looks towards it. The tangent plane at
    the click (origin, ex, ey) is the cylinder's sketch plane.
    """

    def __init__(self, kind: str, origin: Vec, ex: Vec, ey: Vec, n: Vec, *, centre: Vec = None,
                 radius_cm: float = 0.0, radial: Vec = None, sign: float = 1.0, to_world=None):
        self.kind = kind
        self.origin, self.ex, self.ey, self.n = origin, ex, ey, n
        self.centre, self.radius_cm, self.radial, self.sign = centre, radius_cm, radial, sign
        self.to_world = to_world      # (origin, x, y, z) of the occurrence, or None at the root

    @property
    def radius_mm(self) -> float:
        return self.radius_cm * 10.0

    def _radial_at(self, x: float) -> Vec:
        t = x / self.radius_mm
        return _add(_mul(self.radial, math.cos(t)), _mul(self.ex, math.sin(t)))

    def model(self, x: float, y: float, z: float = 0.0) -> Vec:
        """The model point at face coordinates (x, y), z mm out of the face."""
        if self.kind == 'flat':
            return _add(self.origin, _mul(_add(_add(_mul(self.ex, x), _mul(self.ey, y)), _mul(self.n, z)), 0.1))
        r = self.radius_cm + self.sign * z * 0.1
        return _add(_add(self.centre, _mul(self.ey, y * 0.1)), _mul(self._radial_at(x), r))

    def plane(self, x: float, y: float) -> Vec:
        """The point on the sketch plane for face coordinates (x, y)."""
        return _add(self.origin, _mul(_add(_mul(self.ex, x), _mul(self.ey, y)), 0.1))

    def plane_xy(self, p: Vec) -> tuple[float, float]:
        """Face coordinates of a point on the sketch plane."""
        d = _sub(p, self.origin)
        return (_dot(d, self.ex) * 10.0, _dot(d, self.ey) * 10.0)

    def face_xy(self, p: Vec) -> tuple[float, float]:
        """Face coordinates of a point on the face; on a cylinder x lies in (-pi r, pi r]."""
        if self.kind == 'flat':
            return self.plane_xy(p)
        d = _sub(p, self.centre)
        along = _dot(d, self.ey)
        rp = _sub(d, _mul(self.ey, along))
        t = math.atan2(_dot(rp, self.ex), _dot(rp, self.radial))
        return (t * self.radius_mm, along * 10.0)

    def world(self, p: Vec) -> Vec:
        """A component-space point in root space."""
        if self.to_world is None:
            return p
        o, x, y, z = self.to_world
        return (o[0] + p[0] * x[0] + p[1] * y[0] + p[2] * z[0],
                o[1] + p[0] * x[1] + p[1] * y[1] + p[2] * z[1],
                o[2] + p[0] * x[2] + p[1] * y[2] + p[2] * z[2])

    def record(self) -> dict:
        """What a rebake needs to find this face again, in component space."""
        rec = {'kind': self.kind, 'click': list(self.origin), 'normal': list(self.n)}
        if self.kind == 'cylinder':
            rec.update(axis=list(self.ey), centre=list(self.centre), radius=self.radius_cm)
        return rec


def _rigid(matrix: 'adsk.core.Matrix3D') -> tuple:
    """A rigid transform as the images of the origin and the three unit axes."""
    images = []
    for v in ((0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)):
        p = point(v)
        if not p.transformBy(matrix):
            raise DecalError('Fusion could not apply the occurrence transform to a point.')
        images.append(xyz(p))
    o = images[0]
    return (o, _sub(images[1], o), _sub(images[2], o), _sub(images[3], o))


def _state(body, q: Vec, fill) -> str:
    """'solid', 'air' or 'unknown' at q; a point inside one of the fill bodies is solid."""
    p = point(q)
    state = body.pointContainment(p)
    if (state == adsk.fusion.PointContainment.PointInsidePointContainment
            or any(b.pointContainment(p) == adsk.fusion.PointContainment.PointInsidePointContainment for b in fill)):
        return 'solid'
    if state == adsk.fusion.PointContainment.PointOutsidePointContainment:
        return 'air'
    return 'unknown'


def _side(body, p: Vec, n: Vec, fill=()) -> int:
    """+1 when n leaves the solid at p, -1 when it enters it, 0 when neither.

    fill holds inlay bodies of earlier decals, counted as solid, because a decal's clicked
    point often lies over one of its own pockets on the body it was cut into.
    """
    ahead = _state(body, _add(p, _mul(n, PROBE_CM)), fill)
    behind = _state(body, _sub(p, _mul(n, PROBE_CM)), fill)
    if behind == 'solid' and ahead == 'air':
        return 1
    if ahead == 'solid' and behind == 'air':
        return -1
    return 0


def _face_normal(face, p: Vec):
    """The face's outward normal at p from its evaluator, or None. A plane's geometry normal
    ignores the face's orientation, so the top and bottom of a plate report the same one."""
    ok, normal = face.evaluator.getNormalAtPoint(point(p))
    return _unit(xyz(normal)) if ok else None


def _outward(face, body, p: Vec, fill=()) -> Vec:
    """The evaluator's outward normal, checked against the solid: solid behind, air ahead."""
    n = _face_normal(face, p)
    if n is None:
        raise DecalError(f'Fusion gave no normal for the picked face of {body.name} at the clicked point.')
    side = _side(body, p, n, fill)
    if side == 1:
        return n
    if side == -1:
        raise DecalError(f'The normal Fusion gives for the picked face of {body.name} points into the solid '
                         'at the clicked point, so the cut direction is unknown.')
    raise DecalError(f'The picked face of {body.name} does not have solid on one side and air on '
                     'the other at the clicked point. Pick a face on the outside of a solid body.')


def _flat_axes(n: Vec) -> tuple[Vec, Vec]:
    """x and y on a plane: y is the component's +Z laid onto the plane, or x is +X on a
    plane facing up or down. x, y and the outward normal form a right-handed set, so the
    art reads correctly from outside the face."""
    up = (0.0, 0.0, 1.0)
    if abs(_dot(n, up)) < 0.99:
        ey = _unit(_sub(up, _mul(n, _dot(up, n))))
        return _cross(ey, n), ey
    right = (1.0, 0.0, 0.0)
    ex = _unit(_sub(right, _mul(n, _dot(right, n))))
    return ex, _cross(n, ex)


def _axis_sign(a: Vec) -> Vec:
    """The axis direction pointing up the component's Z, else Y, else X."""
    for k in (2, 1, 0):
        if abs(a[k]) > 1e-6:
            return a if a[k] > 0 else _mul(a, -1.0)
    raise ValueError(f'zero-length axis {a}')


def frame_at(face, click_point, fill=()) -> tuple['adsk.fusion.BRepFace', Frame]:
    """The native face and its frame at the clicked point.

    face may be a proxy in an assembly context, with click_point in root space, or a native
    face with click_point in its component's space. fill is passed to the inside or outside
    test, see _side.
    """
    native = face.nativeObject or face
    occ = face.assemblyContext
    p = adsk.core.Point3D.create(click_point.x, click_point.y, click_point.z)
    to_world = None
    if occ is not None:
        to_world = _rigid(occ.transform2)
        inverse = occ.transform2
        if not inverse.invert():
            raise DecalError(f'Fusion could not invert the transform of {occ.fullPathName}.')
        if not p.transformBy(inverse):
            raise DecalError('Fusion could not move the clicked point into the component.')
    body = native.body
    geo = native.geometry
    kind = geo.surfaceType
    c = xyz(p)
    if kind == adsk.core.SurfaceTypes.PlaneSurfaceType:
        n0 = _unit(xyz(geo.normal))
        c = _sub(c, _mul(n0, _dot(_sub(c, xyz(geo.origin)), n0)))
        n = _outward(native, body, c, fill)
        ex, ey = _flat_axes(n)
        return native, Frame('flat', c, ex, ey, n, to_world=to_world)
    if kind == adsk.core.SurfaceTypes.CylinderSurfaceType:
        a = _axis_sign(_unit(xyz(geo.axis)))
        o = xyz(geo.origin)
        centre = _add(o, _mul(a, _dot(_sub(c, o), a)))
        radial = _unit(_sub(c, centre))
        c = _add(centre, _mul(radial, geo.radius))
        n = _outward(native, body, c, fill)
        sign = 1.0 if _dot(n, radial) > 0 else -1.0
        return native, Frame('cylinder', c, _cross(a, n), a, n, centre=centre, radius_cm=geo.radius,
                             radial=radial, sign=sign, to_world=to_world)
    name = _SURFACE_NAMES.get(kind, f'surface type {kind}')
    raise DecalError(f'The picked face is {name}. Pick a flat face or a cylinder.')


# --- Loops -----------------------------------------------------------------------------

def _loop_points(loop) -> list[Vec]:
    """The loop as a closed polyline in component space, without the repeated first point."""
    pts: list[Vec] = []
    for co in loop.coEdges:
        ev = co.edge.evaluator
        ok, t0, t1 = ev.getParameterExtents()
        if not ok:
            raise DecalError('Fusion could not give the parameter range of an edge of the picked face.')
        ok, strokes = ev.getStrokes(t0, t1, STROKE_CM)
        if not ok or len(strokes) < 2:
            raise DecalError('Fusion could not stroke an edge of the picked face.')
        seg = [xyz(q) for q in strokes]
        if co.isOpposedToEdge:
            seg.reverse()
        if pts and _norm(_sub(pts[-1], seg[0])) < SAME_CM:
            seg = seg[1:]
        pts.extend(seg)
    if len(pts) > 1 and _norm(_sub(pts[0], pts[-1])) < SAME_CM:
        pts.pop()
    if len(pts) < 3:
        raise DecalError('A loop of the picked face has fewer than three points after stroking.')
    return pts


def _signed_area(xy: list) -> float:
    return 0.5 * sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(xy, xy[1:] + xy[:1]))


def _probe_inside(xy: list) -> tuple[float, float]:
    """A point just inside the loop, beside the middle of its longest edge."""
    best, a, b = -1.0, xy[0], xy[1]
    for p, q in zip(xy, xy[1:] + xy[:1]):
        d = math.hypot(q[0] - p[0], q[1] - p[1])
        if d > best:
            best, a, b = d, p, q
    dx, dy = (b[0] - a[0]) / best, (b[1] - a[1]) / best
    nx, ny = (-dy, dx) if _signed_area(xy) > 0 else (dy, -dx)
    return ((a[0] + b[0]) / 2 + nx * POCKET_STEP_MM, (a[1] + b[1]) / 2 + ny * POCKET_STEP_MM)


def _is_pocket(frame: Frame, xy: list, bodies) -> bool:
    """True when the inside of the loop, just under the face, lies in one of the bodies.
    The bodies are inlays of the add-in's earlier decals, which fill their own pockets."""
    if not bodies:
        return False
    px, py = _probe_inside(xy)
    p = point(frame.model(px, py, -POCKET_DEPTH_MM))
    inside = adsk.fusion.PointContainment.PointInsidePointContainment
    return any(b.pointContainment(p) == inside for b in bodies)


def _round_hole(loop, frame: Frame):
    """(x, y, diameter) when every edge of the loop is a circle or arc on one centre."""
    circle = adsk.core.Curve3DTypes.Circle3DCurveType
    arc = adsk.core.Curve3DTypes.Arc3DCurveType
    centre, radius = None, 0.0
    for co in loop.coEdges:
        geo = co.edge.geometry
        if geo.curveType not in (circle, arc):
            return None
        c = xyz(geo.center)
        if centre is None:
            centre, radius = c, geo.radius
        elif _norm(_sub(c, centre)) > SAME_CM or abs(geo.radius - radius) > SAME_CM:
            return None
    x, y = frame.face_xy(centre)
    return (x, y, 2.0 * radius * 10.0)


def describe(face, click_point, ignore_bodies=()) -> tuple[Face2D, Frame]:
    """The face flattened for decal_core, and its frame.

    Inner loops whose inside lies in one of ignore_bodies are left out. Those are pockets of
    the add-in's earlier decals on the same body, which the bake suppresses first.
    """
    native, frame = frame_at(face, click_point, ignore_bodies)
    loops = [(lp.isOuter, lp, _loop_points(lp)) for lp in native.loops]
    if frame.kind == 'flat':
        return _flat(frame, loops, ignore_bodies), frame
    return _cylinder(frame, loops, ignore_bodies), frame


def _flat(frame: Frame, loops: list, bodies) -> Face2D:
    outer, inners, holes = None, [], []
    for is_outer, lp, pts in loops:
        xy = [frame.face_xy(p) for p in pts]
        if is_outer:
            outer = xy
        elif not _is_pocket(frame, xy, bodies):
            hole = _round_hole(lp, frame)
            if hole is None:
                inners.append(xy)
            else:
                holes.append(hole)
    if outer is None:
        raise DecalError('Fusion reports no outer loop on the picked face.')
    xs = [x for x, _ in outer]
    return Face2D(kind='flat', outer=outer, inners=inners, holes=holes, centre=(0.0, 0.0),
                  width_mm=max(xs) - min(xs))


def _winding(thetas: list) -> int:
    turn = sum(_wrap(b - a) for a, b in zip(thetas, thetas[1:] + thetas[:1]))
    return int(round(turn / (2 * math.pi)))


def _sweep(xy: list, r_mm: float) -> list:
    """A loop that runs once round the axis, as an open polyline from x = -pi r to +pi r."""
    th = [x / r_mm for x, _ in xy]
    if _winding(th) < 0:
        xy, th = xy[::-1], th[::-1]
    u = [th[0]]
    for a, b in zip(th, th[1:]):
        u.append(u[-1] + _wrap(b - a))
    ys = [y for _, y in xy]
    n = len(u)
    ext = [(u[i % n] + 2 * math.pi * (i // n - 1), ys[i % n]) for i in range(4 * n)]
    out = []
    for (u0, y0), (u1, y1) in zip(ext, ext[1:]):
        if not out:
            if not (u0 <= -math.pi < u1):
                continue
            t = (-math.pi - u0) / (u1 - u0)
            out.append((-math.pi, y0 + t * (y1 - y0)))
        if u1 >= math.pi:
            t = (math.pi - u0) / (u1 - u0)
            out.append((math.pi, y0 + t * (y1 - y0)))
            break
        out.append((u1, y1))
    if len(out) < 2 or out[-1][0] != math.pi:
        raise DecalError('A loop of the picked cylinder runs round the axis but could not be unrolled.')
    return [(t * r_mm, y) for t, y in out]


def _continuous(xy: list, r_mm: float) -> list:
    """A loop unrolled without jumps, starting from its first point's angle."""
    th = [x / r_mm for x, _ in xy]
    u = [th[0]]
    for a, b in zip(th, th[1:]):
        u.append(u[-1] + _wrap(b - a))
    return [(t * r_mm, y) for t, (_, y) in zip(u, xy)]


def _cylinder(frame: Frame, loops: list, bodies) -> Face2D:
    r_mm = frame.radius_mm
    mapped = [(is_outer, [frame.face_xy(p) for p in pts]) for is_outer, _, pts in loops]
    turns = [_winding([x / r_mm for x, _ in xy]) for _, xy in mapped]
    round_loops = [xy for (_, xy), w in zip(mapped, turns) if w != 0]
    others = [(is_outer, xy) for (is_outer, xy), w in zip(mapped, turns) if w == 0]
    if round_loops:
        if len(round_loops) != 2:
            raise DecalError(f'The picked cylinder has {len(round_loops)} edges that run all the way '
                             'round it. The add-in handles a cylinder with one at each end.')
        a, b = (_sweep(xy, r_mm) for xy in round_loops)
        if sum(y for _, y in a) / len(a) > sum(y for _, y in b) / len(b):
            a, b = b, a
        outer = a + b[::-1]
        inners = [_continuous(xy, r_mm) for _, xy in others]
        width = 2 * math.pi * r_mm
    else:
        angles = sorted(x / r_mm for _, xy in mapped for x, _ in xy)
        gaps = [(b - a, a) for a, b in zip(angles, angles[1:])]
        gaps.append((angles[0] + 2 * math.pi - angles[-1], angles[-1]))
        gap, start = max(gaps)
        cut = _wrap(start + gap / 2)
        low = cut - 2 * math.pi if cut > 0 else cut

        def remap(xy):
            out = []
            for x, y in xy:
                t = x / r_mm
                while t <= low:
                    t += 2 * math.pi
                while t > low + 2 * math.pi:
                    t -= 2 * math.pi
                out.append((t * r_mm, y))
            return out

        outer = None
        inners = []
        for is_outer, xy in others:
            if is_outer:
                outer = remap(xy)
            else:
                inners.append(remap(xy))
        if outer is None:
            raise DecalError('Fusion reports no outer loop on the picked cylinder.')
        width = r_mm * (2 * math.pi - gap)
    inners = [xy for xy in inners if not _is_pocket(frame, xy, bodies)]
    return Face2D(kind='cylinder', outer=outer, inners=inners, holes=[], centre=(0.0, 0.0),
                  width_mm=width)


# --- Finding a face again --------------------------------------------------------------

def holds(face, rec: dict, fill=()) -> bool:
    """True when a native face lies on a stored face record's plane or cylinder, looks the same
    way and holds its clicked point. A clicked point over a pocket of one of the fill bodies
    counts as on the face, because the pocket was cut out of that face."""
    click = tuple(rec['click'])
    normal = tuple(rec['normal'])
    geo = face.geometry
    kind = geo.surfaceType
    if rec['kind'] == 'flat':
        if kind != adsk.core.SurfaceTypes.PlaneSurfaceType:
            return False
        n = _unit(xyz(geo.normal))
        if abs(_dot(n, normal)) < PARALLEL or abs(_dot(_sub(click, xyz(geo.origin)), n)) > SAME_CM:
            return False
    else:
        if kind != adsk.core.SurfaceTypes.CylinderSurfaceType:
            return False
        a = _unit(xyz(geo.axis))
        if abs(_dot(a, tuple(rec['axis']))) < PARALLEL or abs(geo.radius - rec['radius']) > SAME_CM:
            return False
        d = _sub(tuple(rec['centre']), xyz(geo.origin))
        if _norm(_sub(d, _mul(a, _dot(d, a)))) > SAME_CM:
            return False
    ok, uv = face.evaluator.getParameterAtPoint(point(click))
    on_face = ok and face.evaluator.isParameterOnFace(uv)
    if not on_face and not (fill and _state(face.body, _sub(click, _mul(normal, PROBE_CM)), fill) == 'solid'):
        return False
    n = _face_normal(face, click)
    return n is not None and _dot(n, normal) > PARALLEL


def match_face(body, rec: dict, fill=()):
    """The face of body that holds a stored face record's clicked point (see holds), or None."""
    for f in body.faces:
        if holds(f, rec, fill):
            return f
    return None


def find_face(body, rec: dict):
    """match_face, raising a DecalError that names the body when nothing matches."""
    f = match_face(body, rec)
    if f is None:
        raise DecalError(f'No face of {body.name} lies on the stored {rec["kind"]} face through the '
                         'stored clicked point. The face has moved or changed since the decal was made.')
    return f


def face_key(face2d: Face2D, frame: Frame, body_name: str) -> tuple:
    """Identity of a face for the dialog: same body, same plane or cylinder, same holes."""
    if frame.kind == 'flat':
        geo = tuple(round(v, 5) for v in frame.n) + (round(_dot(frame.origin, frame.n), 5),)
    else:
        geo = tuple(round(v, 5) for v in frame.ey) + (round(frame.radius_cm, 5),)
    sizes = tuple(sorted(round(d, 1) for _, _, d in face2d.holes))
    return (body_name, frame.kind, geo, sizes)


def same_face(a: Face2D, b: Face2D) -> bool:
    """True when two descriptions of a face give decal_core the same input."""
    if a.kind != b.kind or len(a.inners) != len(b.inners) or len(a.holes) != len(b.holes):
        return False
    if abs(a.width_mm - b.width_mm) > 1e-3:
        return False

    def box(loop):
        xs = [x for x, _ in loop]
        ys = [y for _, y in loop]
        return (min(xs), min(ys), max(xs), max(ys))

    if any(abs(p - q) > 1e-3 for p, q in zip(box(a.outer), box(b.outer))):
        return False
    for p, q in zip(sorted(a.holes), sorted(b.holes)):
        if any(abs(s - t) > 1e-3 for s, t in zip(p, q)):
            return False
    boxes_a = sorted(box(loop) for loop in a.inners)
    boxes_b = sorted(box(loop) for loop in b.inners)
    return all(abs(s - t) <= 1e-3 for p, q in zip(boxes_a, boxes_b) for s, t in zip(p, q))
