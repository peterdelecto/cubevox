"""The bake: one decal's sketch, cut, inlay body, stored cut body and timeline group.

Every feature name is `<decal>` plus a suffix, and the name is chosen before anything is
created, because Fusion silently renames a feature whose name is taken.
"""

import math
import time

import adsk.core
import adsk.fusion

from decal_core.errors import DecalError

from . import Progress
from .face import Frame, point, xyz

DEPTH_PARAMETER = 'inlayDepth'

SKETCH_SUFFIXES = ('', ' point')
FEATURE_SUFFIXES = (' cut', ' inlay', ' body', ' remove')
PLANE_SUFFIXES = (' plane', ' tangent')
BODY_SUFFIXES = (' inlay', ' body')

SLAB_PAD_MM = 0.5          # the flat slab reaches this far past the art on every side
SLAB_REACH_MM = 0.1        # and this far above the face and below the cut floor
MATCH_CELL_MM = 0.05       # grid cell of the profile lookup
MATCH_TOLERANCE_MM = 0.01  # a profile's box corners, or centroid, match a piece's within this
MATCH_AREA = 0.005         # a tie between pieces is broken by area within this fraction
VOLUME_TOLERANCE = 0.001   # inlay volume within 0.1 % of area x depth

APPEARANCE_LIBRARY = 'Fusion Appearance Library'
INLAY_LIBRARY_NAMES = {'black': 'Plastic - Matte (Black)', 'white': 'Plastic - Matte (White)'}


# --- Names -----------------------------------------------------------------------------

def group_named(design, name: str):
    """The timeline group called name, or None."""
    for group in design.timeline.timelineGroups:
        if group.name == name:
            return group
    return None


def _taken(design, component, name: str, reserved, cuts: int) -> bool:
    if name in reserved or group_named(design, name) is not None:
        return True
    if any(component.sketches.itemByName(name + s) is not None for s in SKETCH_SUFFIXES):
        return True
    if any(component.features.itemByName(name + s) is not None for s in FEATURE_SUFFIXES):
        return True
    if any(component.features.itemByName(f'{name} cut {k}') is not None for k in range(1, cuts + 1)):
        return True
    if any(component.constructionPlanes.itemByName(name + s) is not None for s in PLANE_SUFFIXES):
        return True
    return any(component.bRepBodies.itemByName(name + s) is not None for s in BODY_SUFFIXES)


def choose_name(design, component, label: str, reserved=(), cuts: int = 0) -> str:
    """label, or label 2, label 3 ..., the first whose group, sketches, features, planes and
    bodies are all free. cuts is the number of `<decal> cut k` embosses a cylinder will need."""
    base = ' '.join(label.split())[:40] or 'Decal'
    for k in range(1, 1000):
        name = base if k == 1 else f'{base} {k}'
        if not _taken(design, component, name, reserved, cuts):
            return name
    raise DecalError(f'Every name from "{base}" to "{base} 999" is taken in {component.name}.')


def _name(entity, wanted: str, what: str) -> None:
    entity.name = wanted
    if entity.name != wanted:
        raise DecalError(f'Fusion named the new {what} "{entity.name}" instead of "{wanted}".')


# --- Pieces ----------------------------------------------------------------------------

def _ring(loop: list) -> tuple[float, float, float]:
    """Signed area and first moments of a closed loop."""
    a = mx = my = 0.0
    for (x0, y0), (x1, y1) in zip(loop, loop[1:] + loop[:1]):
        c = x0 * y1 - x1 * y0
        a += c
        mx += (x0 + x1) * c
        my += (y0 + y1) * c
    return a / 2.0, mx / 6.0, my / 6.0


def piece_props(piece) -> tuple[float, float, float]:
    """Area (mm2) and centroid of a piece as drawn, holes taken out, whatever the winding."""
    a, mx, my = _ring(piece.outer)
    if a < 0:
        a, mx, my = -a, -mx, -my
    for hole in piece.holes:
        ha, hx, hy = _ring(hole)
        s = 1.0 if ha > 0 else -1.0
        a, mx, my = a - s * ha, mx - s * hx, my - s * hy
    if a <= 0:
        raise DecalError('A traced piece has no area left once its holes are taken out.')
    return a, mx / a, my / a


# --- Steps -----------------------------------------------------------------------------

def _depth_expression(design, depth_mm: float) -> str:
    """inlayDepth when it holds this depth, created when the design has none, else the literal."""
    params = design.userParameters
    param = params.itemByName(DEPTH_PARAMETER)
    if param is None:
        if params.add(DEPTH_PARAMETER, adsk.core.ValueInput.createByString(f'{depth_mm:g} mm'), 'mm',
                      'Inlay depth of decals') is None:
            raise DecalError(f'Fusion did not create the user parameter {DEPTH_PARAMETER}.')
        return DEPTH_PARAMETER
    if abs(param.value * 10.0 - depth_mm) < 1e-6:
        return DEPTH_PARAMETER
    return f'{depth_mm:g} mm'


def _tangent_plane(component, face, frame: Frame, name: str):
    """The plane tangent to the cylinder at the click, through a sketch point placed on a
    plane level with the click. Returns the plane and the scratch features in creation order."""
    planes = component.constructionPlanes
    level_input = planes.createInput()
    if not level_input.setByOffset(component.xYConstructionPlane,
                                   adsk.core.ValueInput.createByReal(frame.origin[2])):
        raise DecalError('Fusion refused the offset plane level with the clicked point.')
    level = planes.add(level_input)
    if level is None:
        raise DecalError('Fusion did not create the plane level with the clicked point.')
    _name(level, name + ' plane', 'construction plane')
    holder = component.sketches.add(level)
    if holder is None:
        raise DecalError('Fusion did not create the sketch for the clicked point.')
    _name(holder, name + ' point', 'sketch')
    q = holder.modelToSketchSpace(point(frame.origin))
    q.z = 0.0
    spot = holder.sketchPoints.add(q)
    if spot is None:
        raise DecalError('Fusion did not create the clicked point in its sketch.')
    tangent_input = planes.createInput()
    if not tangent_input.setByTangentAtPoint(face, spot):
        raise DecalError('Fusion refused the plane tangent to the cylinder at the clicked point.')
    tangent = planes.add(tangent_input)
    if tangent is None:
        raise DecalError('Fusion did not create the plane tangent to the cylinder.')
    _name(tangent, name + ' tangent', 'construction plane')
    scratch = [(level, name + ' plane'), (holder, name + ' point'), (tangent, name + ' tangent')]
    health([e for e, _ in scratch])
    return tangent, scratch


def _line(lines, a, b):
    ln = lines.addByTwoPoints(a, b)
    if ln is None:
        raise DecalError('Fusion did not add a line to the decal sketch.')
    return ln


def _draw_loop(sketch, lines, frame: Frame, loop: list) -> tuple:
    """One closed loop of connected lines; returns its box in sketch space (cm) as
    (x0, y0, x1, y1). Sketch-space z is set to 0, because a point made from model space lands
    a hair off the plane otherwise."""
    if len(loop) < 3:
        raise DecalError('A traced loop has fewer than three points.')
    pts = []
    for x, y in loop:
        q = sketch.modelToSketchSpace(point(frame.plane(x, y)))
        q.z = 0.0
        pts.append(q)
    first = _line(lines, pts[0], pts[1])
    prev = first.endSketchPoint
    for q in pts[2:]:
        prev = _line(lines, prev, q).endSketchPoint
    _line(lines, prev, first.startSketchPoint)
    xs = [q.x for q in pts]
    ys = [q.y for q in pts]
    return (min(xs), min(ys), max(xs), max(ys))


def _draw(sketch, frame: Frame, pieces: list, progress: Progress) -> list:
    """Every piece's outer loop and holes, with the sketch compute deferred so Fusion solves the
    profiles once instead of after every line. Events are pumped once per piece, never per line.
    Returns each piece's outer box in sketch space, for matching profiles.

    On a failure or a cancel the half-drawn sketch is deleted before compute comes back on,
    so nothing waits for a profile solve of a sketch the abort throws away.
    """
    lines = sketch.sketchCurves.sketchLines
    total = len(pieces)
    boxes = []
    sketch.isComputeDeferred = True
    try:
        for i, piece in enumerate(pieces):
            boxes.append(_draw_loop(sketch, lines, frame, piece.outer))
            for hole in piece.holes:
                _draw_loop(sketch, lines, frame, hole)
            progress.step((i + 1) / total)
        progress.begin('Solving the sketch profiles', 46, 50)
    except BaseException:
        sketch.deleteMe()
        raise
    finally:
        if sketch.isValid:
            sketch.isComputeDeferred = False
    return boxes


def _by_area(sketch, frame: Frame, prof, candidates: list, props: list) -> list:
    """The candidates whose area and centroid match the profile. Called only when two pieces
    share a box, because areaProperties costs about 54 ms a profile."""
    ap = prof.areaProperties(adsk.fusion.CalculationAccuracy.HighCalculationAccuracy)
    x, y = frame.plane_xy(xyz(sketch.sketchToModelSpace(ap.centroid)))
    a = ap.area * 100.0
    return [k for k in candidates
            if math.hypot(props[k][1] - x, props[k][2] - y) < MATCH_TOLERANCE_MM
            and abs(props[k][0] - a) < MATCH_AREA * props[k][0]]


def _profiles(sketch, frame: Frame, pieces: list, boxes: list, props: list, progress: Progress) -> list:
    """The profiles that carry a piece, matched by box in sketch space and loop count.

    A profile that carries a piece has the box of that piece's outer loop and one loop per hole
    more than one. A hole's island has the box of the hole, strictly inside, so it matches
    nothing, and count and order never matter. Boxes are compared in sketch space, where Fusion
    gives them, so the sketch's own axes need not line up with the face's.
    """
    tol = MATCH_TOLERANCE_MM / 10.0
    cell = MATCH_CELL_MM / 10.0
    table = {}
    for k, (x0, y0, _, _) in enumerate(boxes):
        table.setdefault((round(x0 / cell), round(y0 / cell)), []).append(k)
    found, chosen = set(), []
    profiles = sketch.profiles
    total = profiles.count
    for i in range(total):
        prof = profiles.item(i)
        box = prof.boundingBox
        lo, hi = box.minPoint, box.maxPoint
        here = (lo.x, lo.y, hi.x, hi.y)
        ki, kj = round(lo.x / cell), round(lo.y / cell)
        candidates = [k for di in (-1, 0, 1) for dj in (-1, 0, 1) for k in table.get((ki + di, kj + dj), ())
                      if k not in found and all(abs(a - b) < tol for a, b in zip(boxes[k], here))]
        if candidates:
            loops = prof.profileLoops.count
            candidates = [k for k in candidates if loops == 1 + len(pieces[k].holes)]
        if len(candidates) > 1:
            candidates = _by_area(sketch, frame, prof, candidates, props)
        if len(candidates) == 1:
            found.add(candidates[0])
            chosen.append(prof)
        progress.step((i + 1) / total)
    if len(found) != len(props):
        raise DecalError(f'The sketch {sketch.name} has profiles matching {len(found)} of the '
                         f'{len(props)} pieces ({total} profiles in all).')
    return chosen


def _extrude_cut(component, sketch, profiles: list, body, frame: Frame, depth_expr: str, name: str):
    """One extrude cut of every profile into the body, away from the outward normal."""
    extrudes = component.features.extrudeFeatures
    collection = adsk.core.ObjectCollection.create()
    for prof in profiles:
        collection.add(prof)
    cut_input = extrudes.createInput(collection, adsk.fusion.FeatureOperations.CutFeatureOperation)
    o = xyz(sketch.sketchToModelSpace(point((0.0, 0.0, 0.0))))
    z = xyz(sketch.sketchToModelSpace(point((0.0, 0.0, 1.0))))
    sketch_normal_dot = sum((z[k] - o[k]) * frame.n[k] for k in range(3))
    direction = (adsk.fusion.ExtentDirections.NegativeExtentDirection if sketch_normal_dot > 0
                 else adsk.fusion.ExtentDirections.PositiveExtentDirection)
    depth = adsk.fusion.DistanceExtentDefinition.create(adsk.core.ValueInput.createByString(depth_expr))
    if not cut_input.setOneSideExtent(depth, direction):
        raise DecalError('Fusion refused the depth of the cut.')
    cut_input.participantBodies = [body]
    feature = extrudes.add(cut_input)
    if feature is None:
        raise DecalError(f'Fusion did not create the cut from the sketch {sketch.name}.')
    _name(feature, name + ' cut', 'cut')
    health([feature])
    return [feature]


def _emboss_cuts(component, profiles: list, face, depth_expr: str, name: str, progress: Progress) -> list:
    """One emboss per profile into the cylinder, named `<decal> cut 1`, `cut 2` ... Fusion's
    emboss refuses several profiles at once unless they sit level along the axis, and text and
    art never do."""
    embosses = component.features.embossFeatures
    depth = '-' + depth_expr
    cuts = []
    for k, prof in enumerate(profiles, start=1):
        cut_input = embosses.createInput([prof], [face], adsk.core.ValueInput.createByString(depth))
        if cut_input is None:
            raise DecalError(f'Fusion refused the emboss input for piece {k} of {name}.')
        cut_input.isTangentChain = False
        feature = embosses.add(cut_input)
        if feature is None:
            raise DecalError(f'Fusion did not create the emboss for piece {k} of {name}.')
        _name(feature, f'{name} cut {k}', 'emboss')
        health([feature])
        cuts.append(feature)
        progress.step(k / len(profiles))
    return cuts


def _delete_together(design, entities: list, label: str) -> None:
    """Delete timeline neighbours with one call, through a temporary group deleted with its
    contents."""
    indices = sorted(e.timelineObject.index for e in entities)
    if indices != list(range(indices[0], indices[0] + len(indices))):
        raise DecalError(f'The scratch features of {label} are not next to each other in the timeline '
                         f'(positions {indices}), so they cannot be deleted together.')
    group = design.timeline.timelineGroups.add(indices[0], indices[-1])
    if group is None:
        raise DecalError(f'Fusion did not group the scratch features of {label} for deletion.')
    if not group.deleteMe(True):
        raise DecalError(f'Fusion did not delete the scratch features of {label}.')
    left = [e.name for e in entities if e.isValid]
    if left:
        raise DecalError(f'Fusion left {", ".join(left)} after deleting the scratch features of {label}.')


def _slab_box(frame: Frame, pieces: list, depth: float):
    xs = [x for p in pieces for x, _ in p.outer]
    ys = [y for p in pieces for _, y in p.outer]
    x0, x1 = min(xs) - SLAB_PAD_MM, max(xs) + SLAB_PAD_MM
    y0, y1 = min(ys) - SLAB_PAD_MM, max(ys) + SLAB_PAD_MM
    height = depth + 2 * SLAB_REACH_MM
    centre = point(frame.model((x0 + x1) / 2, (y0 + y1) / 2, -depth / 2))
    box = adsk.core.OrientedBoundingBox3D.create(
        centre, adsk.core.Vector3D.create(*frame.ex), adsk.core.Vector3D.create(*frame.ey),
        (x1 - x0) / 10.0, (y1 - y0) / 10.0, height / 10.0)
    return adsk.fusion.TemporaryBRepManager.get().createBox(box)


def _inlay(before, body, frame: Frame, pieces: list, depth: float):
    """The material the cut removed, as one temporary body of many lumps.

    Flat: (slab over the art ∩ body before the cut) − body after the cut. Cylinder: before −
    after. Never a second extrude of the profiles and never a body-against-inlay check, both
    of which ran for tens of minutes on the hand-built decals.
    """
    tmp = adsk.fusion.TemporaryBRepManager.get()
    after = tmp.copy(body)
    if after is None:
        raise DecalError(f'Fusion did not copy {body.name} after the cut.')
    if frame.kind == 'flat':
        slab = _slab_box(frame, pieces, depth)
        if slab is None:
            raise DecalError('Fusion did not create the slab over the art.')
        if not tmp.booleanOperation(slab, before, adsk.fusion.BooleanTypes.IntersectionBooleanType):
            raise DecalError('The intersection of the slab with the body before the cut failed.')
        if not tmp.booleanOperation(slab, after, adsk.fusion.BooleanTypes.DifferenceBooleanType):
            raise DecalError('Subtracting the cut body from the slab failed.')
        return slab
    if not tmp.booleanOperation(before, after, adsk.fusion.BooleanTypes.DifferenceBooleanType):
        raise DecalError('Subtracting the cut body from the body before the cut failed.')
    return before


def _check(body, pieces: int, expected_mm3: float, what: str) -> tuple[int, float]:
    lumps = body.lumps.count
    volume = body.volume * 1000.0
    if lumps != pieces or abs(volume - expected_mm3) > VOLUME_TOLERANCE * expected_mm3:
        raise DecalError(f'{what} has {lumps} lumps and {volume:.2f} mm³. The decal expects {pieces} '
                         f'lumps and {expected_mm3:.2f} mm³ (area times depth, within 0.1 %).')
    return lumps, volume


def _base_body(component, temporary, label: str):
    """A base feature holding the temporary body whole, so its lumps stay one body."""
    feature = component.features.baseFeatures.add()
    if feature is None:
        raise DecalError(f'Fusion did not create the base feature {label}.')
    if not feature.startEdit():
        raise DecalError(f'Fusion did not open the base feature {label} for editing.')
    try:
        added = component.bRepBodies.add(temporary, feature)
    finally:
        finished = feature.finishEdit()
    if added is None:
        raise DecalError(f'Fusion did not add the body to the base feature {label}.')
    if not finished:
        raise DecalError(f'Fusion did not close the base feature {label}.')
    _name(feature, label, 'base feature')
    bodies = [feature.bodies.item(i) for i in range(feature.bodies.count) if feature.bodies.item(i).faces.count > 0]
    if len(bodies) != 1:
        raise DecalError(f'The base feature {label} holds {len(bodies)} bodies instead of one.')
    _name(bodies[0], label, 'body')
    return feature, bodies[0]


def _copy_look(source, target) -> None:
    if source.material is not None:
        target.material = source.material
    if source.appearanceSourceType == adsk.core.AppearanceSourceTypes.OverrideAppearanceSource:
        target.appearance = source.appearance


def inlay_appearance(design, colour: str):
    """The design's appearance `Decal inlay <colour>`, copied once from the Fusion Appearance
    Library. Every decal of that colour shares it, so recolouring it recolours them all."""
    if colour not in INLAY_LIBRARY_NAMES:
        raise DecalError(f'The inlay colour {colour!r} is not black or white.')
    name = f'Decal inlay {colour}'
    found = design.appearances.itemByName(name)
    if found is not None:
        return found
    library = adsk.core.Application.get().materialLibraries.itemByName(APPEARANCE_LIBRARY)
    if library is None:
        raise DecalError(f'Fusion has no library called {APPEARANCE_LIBRARY} to copy the {colour} inlay '
                         'appearance from.')
    appearances = library.appearances
    source = appearances.itemByName(INLAY_LIBRARY_NAMES[colour])
    if source is None:
        for i in range(appearances.count):
            candidate = appearances.item(i)
            words = candidate.name.lower()
            if 'matte' in words and colour in words:
                source = candidate
                break
    if source is None:
        raise DecalError(f'The {APPEARANCE_LIBRARY} has no matte {colour} appearance to copy for the inlay.')
    made = design.appearances.addByCopy(source, name)
    if made is None:
        raise DecalError(f'Fusion did not copy {source.name} into the design as {name}.')
    return made


def _delete(entity, label: str) -> None:
    if not entity.deleteMe():
        raise DecalError(f'Fusion did not delete {label}.')


def health(entities) -> list:
    """Warnings of the entities' timeline objects. An error raises with Fusion's message."""
    warnings = []
    for entity in entities:
        item = entity.timelineObject
        state = item.healthState
        if state == adsk.fusion.FeatureHealthStates.ErrorFeatureHealthState:
            raise DecalError(f'{item.name} failed in Fusion: {item.errorOrWarningMessage or "no message"}')
        if state == adsk.fusion.FeatureHealthStates.WarningFeatureHealthState:
            warnings.append(f'{item.name}: {item.errorOrWarningMessage}')
    return warnings


def _group(design, kept: list, name: str):
    indices = sorted(e.timelineObject.index for e in kept)
    if indices != list(range(indices[0], indices[0] + len(indices))):
        raise DecalError(f'The features of {name} are not next to each other in the timeline '
                         f'(positions {indices}), so they cannot be grouped.')
    group = design.timeline.timelineGroups.add(indices[0], indices[-1])
    if group is None:
        raise DecalError(f'Fusion did not create the timeline group {name}.')
    _name(group, name, 'timeline group')
    return group


# --- The bake --------------------------------------------------------------------------

def bake(design, component, body, face, frame: Frame, result, settings: dict, name: str,
         progress: Progress = None) -> dict:
    """Cut the result's pieces into body at face and build the inlay. Returns counts, the
    inlay volume, the feature names and the seconds per stage.

    body and face are native to component, and frame is in component space. The progress
    bar runs from 15 % to 100 %; the caller uses the first 15 % for the source and trace.
    """
    progress = progress or Progress()
    seconds = {}
    clock = time.perf_counter()

    def lap(stage: str) -> None:
        nonlocal clock
        now = time.perf_counter()
        seconds[stage] = round(now - clock, 2)
        clock = now

    pieces = list(result.pieces)
    if not pieces:
        raise DecalError('The plan has no pieces to cut.')
    depth = float(settings['depth_mm'])
    props = [piece_props(p) for p in pieces]
    area = sum(a for a, _, _ in props)
    expected = area * depth
    if frame.kind == 'cylinder':
        # A pocket wrapped round radius r, depth d deep, holds area x d x (1 - d / 2r) when the
        # face looks away from the axis and (1 + d / 2r) when it looks towards it.
        expected *= 1.0 - frame.sign * depth / (2.0 * frame.radius_mm)
    depth_expr = _depth_expression(design, depth)
    tmp = adsk.fusion.TemporaryBRepManager.get()

    scratch = []
    plane = face
    if frame.kind == 'cylinder':
        progress.begin('Placing the tangent plane', 15, 16)
        plane, scratch = _tangent_plane(component, face, frame, name)
    progress.begin('Drawing the sketch', 16, 46)
    sketch = component.sketches.add(plane)
    if sketch is None:
        raise DecalError(f'Fusion did not create the sketch {name}.')
    _name(sketch, name, 'sketch')
    boxes = _draw(sketch, frame, pieces, progress)
    lap('sketch')

    progress.begin('Matching profiles to pieces', 50, 60)
    profiles = _profiles(sketch, frame, pieces, boxes, props, progress)
    lap('profiles')

    progress.begin('Cutting', 60, 68)
    before = tmp.copy(body)
    if before is None:
        raise DecalError(f'Fusion did not copy {body.name} before the cut.')
    volume_before = body.volume * 1000.0
    if frame.kind == 'flat':
        cuts = _extrude_cut(component, sketch, profiles, body, frame, depth_expr, name)
    else:
        cuts = _emboss_cuts(component, profiles, face, depth_expr, name, progress)
    removed = volume_before - body.volume * 1000.0
    if abs(removed - expected) > VOLUME_TOLERANCE * expected:
        raise DecalError(f'The cut of {name} took {removed:.2f} mm³ out of {body.name}. The decal expects '
                         f'{expected:.2f} mm³, so the cut went the wrong way or missed pieces.')
    lap('cut')

    progress.begin('Building the inlay body', 68, 82)
    inlay_temporary = _inlay(before, body, frame, pieces, depth)
    _check(inlay_temporary, len(pieces), expected, 'The temporary inlay body')
    inlay_feature, inlay = _base_body(component, inlay_temporary, name + ' inlay')
    lumps, volume = _check(inlay, len(pieces), expected, f'The body {name} inlay')
    inlay.appearance = inlay_appearance(design, settings['colour'])
    lap('inlay')

    stored_name = None
    if settings['store_cut']:
        progress.begin('Storing the cut body', 82, 94)
        copy = tmp.copy(body)
        if copy is None:
            raise DecalError(f'Fusion did not copy {body.name} after the cut.')
        stored_feature, stored = _base_body(component, copy, name + ' body')
        _copy_look(body, stored)
        remove = component.features.removeFeatures.add(body)
        if remove is None:
            raise DecalError(f'Fusion did not remove {body.name} after storing its copy.')
        _name(remove, name + ' remove', 'remove feature')

        if frame.kind == 'flat':
            # The cut goes before its sketch, because Fusion will not delete a sketch that a
            # feature still uses.

            _delete(cuts[0], name + ' cut')
            _delete(sketch, name)
        else:
            # A cylinder leaves three scratch features, the sketch and one emboss per piece;
            # deleting them one by one cost 0.24 s each in Fusion.

            _delete_together(design, [e for e, _ in scratch] + [sketch] + cuts, name)
        kept = [inlay_feature, stored_feature, remove]
        stored_name = name + ' body'
        lap('store')
    else:
        kept = [e for e, _ in scratch] + [sketch] + cuts + [inlay_feature]

    progress.begin('Grouping the timeline', 94, 100)
    _group(design, kept, name)
    warnings = health(kept)
    lap('group')
    progress.step(1.0)
    return {
        'name': name,
        'kind': frame.kind,
        'pieces': len(pieces),
        'points': result.points,
        'lumps': lumps,
        'inlay_volume_mm3': round(volume, 3),
        'expected_volume_mm3': round(expected, 3),
        'drawn_area_mm2': round(area, 3),
        'component_bodies': component.bRepBodies.count,
        'features': [e.timelineObject.name for e in kept],
        'cut_features': len(cuts),
        'features_made': len(scratch) + 1 + len(cuts) + 1 + (2 if settings['store_cut'] else 0),
        'inlay_body': name + ' inlay',
        'stored_body': stored_name,
        'warnings': warnings,
        'seconds': seconds,
    }
