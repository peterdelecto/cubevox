"""Entry points shared by the dialog and the headless check.

run_decal(design, face, click_point, settings, source) makes one decal; rebake(design, name)
remakes one from its stored record. settings is the dict the dialog builds, keyed exactly as
decal_core.plan.DEFAULTS. source is one of

    {'kind': 'png', 'path': '/abs/file.png'}
    {'kind': 'svg', 'path': '/abs/file.svg'}
    {'kind': 'text', 'text': 'cubevox', 'font': 'Arial', 'bold': False, 'italic': False}

Both suppress the add-in's other live decals on the same body first, so the new cut starts
from the body as it was before any of them.
"""

import datetime
import math
import os
import time

import adsk.core
import adsk.fusion

from decal_core import plan, png
from decal_core.errors import DecalError

from . import Progress, log
from . import bake as bake_mod
from . import face as face_mod
from . import store

PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'
SKETCH_STROKE_CM = 0.008   # SVG and text curves read back as polylines at 0.08 mm
JOIN_MM = 1e-3             # curve ends closer than this join
TEXT_HEIGHT_CM = 1.0       # scratch text height; decal_core rescales to the chosen width

CURVES_PER_PUMP = 100      # the scratch sketch read pumps events once per this many curves

# Settings the Fusion side adds to decal_core's; decal_core ignores them.
FUSION_DEFAULTS = {'colour': 'black'}

# Settings that change the traced pieces; depth, the stored cut and the colour leave them as they are.
RESULT_KEYS = tuple(k for k in plan.DEFAULTS if k not in ('depth_mm', 'store_cut'))


# --- Settings and sources ---------------------------------------------------------------

def normalise_settings(settings: dict) -> dict:
    """The dialog's settings with defaults filled in and every value of the right type."""
    unknown = sorted(set(settings) - set(plan.DEFAULTS) - set(FUSION_DEFAULTS))
    if unknown:
        raise DecalError(f'The settings name keys the add-in does not know: {", ".join(unknown)}.')
    cfg = dict(plan.DEFAULTS)
    cfg.update(FUSION_DEFAULTS)
    cfg.update(settings)
    if cfg['colour'] not in bake_mod.INLAY_LIBRARY_NAMES:
        raise DecalError(f'The inlay colour {cfg["colour"]!r} is not black or white.')
    for key in ('offset_x', 'offset_y', 'min_width_mm', 'min_area_mm2', 'margin_mm', 'depth_mm'):
        cfg[key] = float(cfg[key])
    for key in ('invert', 'mirror', 'keepouts', 'store_cut'):
        cfg[key] = bool(cfg[key])
    cfg['threshold'] = int(cfg['threshold'])
    cfg['rotation'] = int(cfg['rotation'])
    cfg['width_mm'] = None if cfg['width_mm'] is None else float(cfg['width_mm'])
    cfg['hole_keepouts'] = {round(float(k), 1): float(v) for k, v in cfg['hole_keepouts'].items()}
    if cfg['mode'] not in ('dark', 'shading'):
        raise DecalError(f'The trace mode {cfg["mode"]!r} is not dark or shading.')
    if cfg['depth_mm'] <= 0:
        raise DecalError(f'The depth {cfg["depth_mm"]:g} mm is not more than zero.')
    if cfg['width_mm'] is not None and cfg['width_mm'] <= 0:
        raise DecalError(f'The width {cfg["width_mm"]:g} mm is not more than zero.')
    if min(cfg['min_width_mm'], cfg['min_area_mm2'], cfg['margin_mm']) < 0:
        raise DecalError('The minimum width, minimum area and margin cannot be negative.')
    return cfg


def result_settings(cfg: dict) -> dict:
    return {k: cfg[k] for k in RESULT_KEYS}


def source_key(source: dict) -> tuple:
    """What identifies a source's content: the file's path, size and time, or the text."""
    kind = source['kind']
    if kind in ('png', 'svg'):
        path = source['path']
        try:
            st = os.stat(path)
        except OSError as e:
            raise DecalError(f'The file {path} cannot be read: {e.strerror}.') from e
        return (kind, path, st.st_mtime_ns, st.st_size)
    if kind == 'text':
        return ('text', source['text'], source.get('font') or '', bool(source.get('bold')),
                bool(source.get('italic')))
    raise DecalError(f'The source kind {kind!r} is not png, svg or text.')


def source_label(source: dict) -> str:
    """The decal's name before any number is added: the file's stem or the text."""
    if source['kind'] == 'text':
        return source['text']
    return os.path.splitext(os.path.basename(source['path']))[0]


def _header(path: str, size: int) -> bytes:
    try:
        with open(path, 'rb') as fh:
            return fh.read(size)
    except OSError as e:
        raise DecalError(f'The file {path} cannot be read: {e.strerror}.') from e


def load_source(design, source: dict, progress: Progress = None):
    """The source as a decal_core Source, and notes for the readout.

    A PNG is decoded in Python. An SVG or a text goes through a scratch sketch on the root
    component's XY plane, which is deleted again before this returns.
    """
    progress = progress or Progress()
    kind = source['kind']
    if kind == 'png':
        path = source['path']
        head = _header(path, len(PNG_SIGNATURE))
        if head != PNG_SIGNATURE:
            raise DecalError(f'The file {path} is not a PNG. Its first bytes are {head!r}.')
        w, h, grey, alpha = png.read(path)
        return plan.Source(kind='image', w=w, h=h, grey=grey, alpha=alpha), []
    if kind == 'svg':
        path = source['path']
        head = _header(path, 4096)
        if b'<svg' not in head.lower():
            raise DecalError(f'The file {path} is not an SVG. Its first 4 KB hold no <svg tag and '
                             f'start with {head[:40]!r}.')
        loops, notes = _sketch_loops(design, lambda sk: _import_svg(sk, path), f'The SVG {path}', progress)
        return plan.Source(kind='loops', loops=loops), notes
    if kind == 'text':
        text = ' '.join(str(source['text']).split())
        if not text:
            raise DecalError('The text is empty, so there is nothing to cut.')
        font = source.get('font') or ''
        if font and font not in list(adsk.core.Application.get().fontNames):
            raise DecalError(f'The font {font} is not installed in Fusion.')
        bold, italic = bool(source.get('bold')), bool(source.get('italic'))
        loops, notes = _sketch_loops(design, lambda sk: _add_text(sk, text, font, bold, italic),
                                     f'The text "{text}"', progress)
        return plan.Source(kind='loops', loops=loops), notes
    raise DecalError(f'The source kind {kind!r} is not png, svg or text.')


def _import_svg(sketch, path: str) -> list:
    if not sketch.importSVG(path, 0.0, 0.0, 1.0):
        raise DecalError(f'Fusion could not import the SVG {path}.')
    curves = sketch.sketchCurves
    return [curves.item(i) for i in range(curves.count)]


def _add_text(sketch, text: str, font: str, bold: bool, italic: bool) -> list:
    texts = sketch.sketchTexts
    if "'" in text:
        # createInput3 takes an expression with the text in single quotes; the retired
        # createInput2 takes the text as it is, so it carries texts holding a quote.
        text_input = texts.createInput2(text, TEXT_HEIGHT_CM)
    else:
        text_input = texts.createInput3(f"'{text}'", adsk.core.ValueInput.createByReal(TEXT_HEIGHT_CM))
    if text_input is None:
        raise DecalError(f'Fusion refused the text "{text}".')
    if font:
        text_input.fontName = font
    style = 0
    if bold:
        style |= adsk.fusion.TextStyles.TextStyleBold
    if italic:
        style |= adsk.fusion.TextStyles.TextStyleItalic
    text_input.textStyle = style
    width = 2.0 * TEXT_HEIGHT_CM * (len(text) + 2)
    if not text_input.setAsMultiLine(adsk.core.Point3D.create(0.0, 0.0, 0.0),
                                     adsk.core.Point3D.create(width, 2.0 * TEXT_HEIGHT_CM, 0.0),
                                     adsk.core.HorizontalAlignments.LeftHorizontalAlignment,
                                     adsk.core.VerticalAlignments.BottomVerticalAlignment, 0.0):
        raise DecalError(f'Fusion refused the text box for "{text}".')
    sketch_text = texts.add(text_input)
    if sketch_text is None:
        raise DecalError(f'Fusion did not lay out the text "{text}".')

    # Only the exploded glyph curves are art. The text box also draws its own rectangle.

    curves = sketch_text.explode()
    if not curves:
        raise DecalError(f'Fusion gave no curves for the text "{text}".')
    return list(curves)


def _sketch_loops(design, fill, what: str, progress: Progress):
    """Closed loops from the curves fill() puts in a scratch sketch, and notes."""
    root = design.rootComponent
    sketch = root.sketches.add(root.xYConstructionPlane)
    if sketch is None:
        raise DecalError('Fusion did not create the scratch sketch.')

    # Compute stays deferred for the whole life of the scratch sketch: its curves are read as
    # geometry and its profiles are never needed. Deleting it ends the deferral; compute is
    # turned back on only when the delete fails and the sketch stays.

    sketch.isComputeDeferred = True
    try:
        loops, open_curves = _read_loops(fill(sketch), progress)
    finally:
        if sketch.isValid and not sketch.deleteMe():
            sketch.isComputeDeferred = False
            raise DecalError('Fusion did not delete the scratch sketch.')
    if not loops:
        raise DecalError(f'{what} has no closed loop. Fusion found {open_curves} open curves.')
    notes = [f'{open_curves} open curves were left out.'] if open_curves else []
    return loops, notes


def _curve_points(curve) -> list:
    """A sketch curve as points in mm, sketch space, y up."""
    geo = curve.geometry
    if geo.curveType == adsk.core.Curve3DTypes.Line3DCurveType:
        s, e = geo.startPoint, geo.endPoint
        return [(s.x * 10.0, s.y * 10.0), (e.x * 10.0, e.y * 10.0)]
    ev = geo.evaluator
    ok, t0, t1 = ev.getParameterExtents()
    if not ok:
        raise DecalError('Fusion could not give the parameter range of a curve in the scratch sketch.')
    ok, pts = ev.getStrokes(t0, t1, SKETCH_STROKE_CM)
    if not ok or len(pts) < 2:
        raise DecalError('Fusion could not stroke a curve in the scratch sketch.')
    return [(p.x * 10.0, p.y * 10.0) for p in pts]


def _read_loops(curves: list, progress: Progress):
    total = len(curves)
    closed, open_parts = [], []
    for i, curve in enumerate(curves):
        if curve.isConstruction:
            continue
        pts = _curve_points(curve)
        if len(pts) > 3 and math.dist(pts[0], pts[-1]) <= JOIN_MM:
            closed.append(pts[:-1])
        else:
            open_parts.append(pts)
        if (i + 1) % CURVES_PER_PUMP == 0:
            progress.step((i + 1) / total)
    joined, open_curves = _chain(open_parts)
    return closed + joined, open_curves


def _chain(parts: list):
    """Join open polylines end to end into closed loops. Returns the loops and how many
    curves sat in chains that never closed."""
    cell = 10 * JOIN_MM
    ends = {}

    def key(p):
        return (round(p[0] / cell), round(p[1] / cell))

    for i, part in enumerate(parts):
        for end, p in ((0, part[0]), (1, part[-1])):
            ends.setdefault(key(p), []).append((i, end))

    def partner(p, used):
        kx, ky = key(p)
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for i, end in ends.get((kx + dx, ky + dy), ()):
                    if i not in used:
                        q = parts[i][0] if end == 0 else parts[i][-1]
                        if math.dist(p, q) <= JOIN_MM:
                            return i, end
        return None

    used, loops, open_curves = set(), [], 0
    for i in range(len(parts)):
        if i in used:
            continue
        used.add(i)
        chain, count, flipped = list(parts[i]), 1, False
        while True:
            if len(chain) > 3 and math.dist(chain[0], chain[-1]) <= JOIN_MM:
                loops.append(chain[:-1])
                break
            nxt = partner(chain[-1], used)
            if nxt is None:
                if flipped:
                    open_curves += count
                    break
                chain.reverse()
                flipped = True
                continue
            j, end = nxt
            used.add(j)
            count += 1
            seg = parts[j] if end == 0 else parts[j][::-1]
            chain.extend(seg[1:])
    return loops, open_curves


# --- Earlier decals ---------------------------------------------------------------------

def live_decals(design, component, base_name: str, exclude: str = None) -> list:
    """Records of the add-in's unsuppressed decals cut from base_name in component."""
    out = []
    for name in store.names(design):
        if name == exclude:
            continue
        rec = store.load(design, name)
        if rec is None or rec['face']['component'] != component.name or rec['face']['body'] != base_name:
            continue
        group = bake_mod.group_named(design, name)
        if group is not None and not group.isSuppressed:
            out.append(rec)
    return out


def inlay_bodies(component, records: list) -> list:
    """The inlay bodies of the given decal records that are in the model."""
    bodies = (component.bRepBodies.itemByName(rec['inlay_body']) for rec in records)
    return [b for b in bodies if b is not None]


def _suppress_live(design, component, base_name: str, exclude: str = None) -> list:
    names = []
    for rec in live_decals(design, component, base_name, exclude):
        group = bake_mod.group_named(design, rec['name'])
        group.isSuppressed = True
        if not group.isSuppressed:
            raise DecalError(f'Fusion did not suppress the group {rec["name"]}.')
        names.append(rec['name'])
    return names


def suppress_others(design, body, name: str = None) -> list:
    """Suppress the add-in's live decals on body, except name. Hand-made groups are left
    alone. Returns the names suppressed."""
    native = body.nativeObject or body
    base = store.base_of(design, native)
    return _suppress_live(design, native.parentComponent, base, exclude=name)


# --- Running ----------------------------------------------------------------------------

def _check_design(design) -> None:
    if design is None:
        raise DecalError('The active document is not a Fusion design.')
    if design.designType != adsk.fusion.DesignTypes.ParametricDesignType:
        raise DecalError('The design does not capture history. A decal needs the timeline.')
    timeline = design.timeline
    if timeline.markerPosition != timeline.count:
        raise DecalError('The timeline marker is not at the end of the timeline. A decal is only '
                         'added at the end.')


def _component(design, name: str):
    component = design.allComponents.itemByName(name)
    if component is None:
        raise DecalError(f'The design has no component called {name}.')
    return component


def _occurrence(design, component, path: str):
    """The occurrence the decal was made through, or the component's first, or None at the root."""
    if component.name == design.rootComponent.name:
        return None
    occurrences = design.rootComponent.allOccurrencesByComponent(component)
    for i in range(occurrences.count):
        if occurrences.item(i).fullPathName == path:
            return occurrences.item(i)
    return occurrences.item(0) if occurrences.count else None


def run_decal(design, face, click_point, settings: dict, source: dict, progress: Progress = None,
              cached: dict = None) -> dict:
    """Make one decal on the face at the clicked point and return the bake's report.

    face may be a selection proxy with click_point in root space, or a native face with
    click_point in its component's space. cached is the dialog's last plan as a dict with
    face2d, source_key, settings, result and notes; it is used when the face, the source and
    the settings that shape the pieces are all unchanged.
    """
    started = time.perf_counter()
    design = adsk.fusion.Design.cast(design)
    _check_design(design)
    return _make(design, face, click_point, normalise_settings(settings), source, progress or Progress(),
                 cached, None, started)


def rebake(design, name: str, settings: dict = None, source: dict = None, progress: Progress = None,
           cached: dict = None, face=None, click_point=None) -> dict:
    """Make a decal again from its record, under its own name. settings and source override the
    stored ones where given. face and click_point, given together, put it on that face (the
    dialog passes the face it shows); otherwise the stored face is found again. The design
    attribute keeps the record, so a suppressed decal can be rebaked too. The face, the source
    and the plan are checked before the old group is deleted."""
    started = time.perf_counter()
    design = adsk.fusion.Design.cast(design)
    _check_design(design)
    rec = store.load(design, name)
    if rec is None:
        raise DecalError(f'The design has no decal called {name}.')
    cfg = normalise_settings({**rec['settings'], **(settings or {})})
    src = dict(source or rec['source'])
    if (face is None) != (click_point is None):
        raise DecalError('A rebake takes a face and a clicked point together, or neither.')
    if face is None:
        face, click_point, _ = target_face(design, rec)
    return _make(design, face, click_point, cfg, src, progress or Progress(), cached, name, started)


def _make(design, face, click_point, cfg: dict, source: dict, progress: Progress, cached, replace,
          started: float) -> dict:
    """Plan on the face as picked, then change the design: delete the decal being replaced,
    suppress the add-in's other live decals on the body, find the face again on the base body
    and bake under the replaced decal's name or a new one."""
    occurrence = face.assemblyContext.fullPathName if face.assemblyContext is not None else ''
    body = (face.nativeObject or face).body
    component = body.parentComponent
    inlay = store.inlay_of(design, body)
    if inlay is not None:
        raise DecalError(f'The picked face is on {body.name}, the inlay body of decal {inlay}. '
                         'Pick a face on the part.')
    base_name = store.base_of(design, body)
    fill = inlay_bodies(component, live_decals(design, component, base_name))
    planned = _plan(design, face, click_point, fill, cfg, source, progress, cached)

    # The plan is valid. Only from here on does the design change.

    if replace is not None:
        group = bake_mod.group_named(design, replace)
        if group is not None and not group.deleteMe(True):
            raise DecalError(f'Fusion did not delete the group {replace}.')
        store.forget(design, replace)
    suppressed = _suppress_live(design, component, base_name)
    if replace is not None or suppressed or body.name != base_name:
        planned = _replan_on_base(design, component, base_name, planned, cfg, source, progress, suppressed)
    return _commit(design, component, base_name, planned.face, planned, cfg, source, progress, replace,
                   occurrence, suppressed, started)


class _Plan:
    """A face described as picked and the pieces planned on it."""

    def __init__(self, face, face2d, frame, result, notes, src, reused, source_seconds, trace_seconds):
        self.face, self.face2d, self.frame, self.result, self.notes = face, face2d, frame, result, notes
        self.src, self.reused = src, reused
        self.source_seconds, self.trace_seconds = source_seconds, trace_seconds


def _plan(design, face, click_point, fill, cfg: dict, source: dict, progress: Progress, cached) -> _Plan:
    """Describe the face as picked and plan the pieces, leaving the design as it was. An SVG or
    text passes through a scratch sketch that is deleted again. Raises DecalError for a face,
    source or plan that cannot be cut, including a passed ceiling."""
    progress.begin('Reading the face', 0, 3)
    face2d, frame = face_mod.describe(face, click_point, fill)
    progress.begin('Reading the source', 3, 8)
    key = source_key(source)
    reused = (cached is not None and cached['source_key'] == key
              and result_settings(normalise_settings(cached['settings'])) == result_settings(cfg)
              and face_mod.same_face(cached['face2d'], face2d))
    clock = time.perf_counter()
    if reused:
        return _Plan(face.nativeObject or face, face2d, frame, cached['result'], list(cached.get('notes', [])),
                     None, True, 0.0, 0.0)
    src, notes = load_source(design, source, progress)
    source_seconds = time.perf_counter() - clock
    progress.begin('Tracing the art', 8, 15)
    clock = time.perf_counter()
    result = plan.compute(src, face2d, cfg)
    if result.over:
        raise DecalError(result.over)
    return _Plan(face.nativeObject or face, face2d, frame, result, notes, src, False, source_seconds,
                 time.perf_counter() - clock)


def _unsuppress(design, names: list) -> None:
    for name in names:
        group = bake_mod.group_named(design, name)
        if group is not None:
            group.isSuppressed = False
            if group.isSuppressed:
                raise DecalError(f'Fusion did not unsuppress the group {name}.')


def _replan_on_base(design, component, base_name: str, planned: _Plan, cfg: dict, source: dict,
                    progress: Progress, suppressed: list) -> _Plan:
    """The same face found again on the base body once earlier decals are suppressed. The plan
    stands when the face reads the same; otherwise it is made again on the face as it now is.
    A failure here unsuppresses the decals again before it raises."""
    try:
        base = component.bRepBodies.itemByName(base_name)
        if base is None:
            raise DecalError(f'The body {base_name} is not in {component.name} after suppressing '
                             f'{", ".join(suppressed) or "nothing"}.')
        face_rec = planned.frame.record()
        native = face_mod.find_face(base, face_rec)
        face2d, frame = face_mod.describe(native, face_mod.point(tuple(face_rec['click'])))
        if face_mod.same_face(planned.face2d, face2d):
            return _Plan(native, face2d, frame, planned.result, planned.notes, planned.src, planned.reused,
                         planned.source_seconds, planned.trace_seconds)
        log(f'face of {base_name} reads differently once earlier decals are suppressed; planning again')
        src = planned.src if planned.src is not None else load_source(design, source, progress)[0]
        result = plan.compute(src, face2d, cfg)
        if result.over:
            raise DecalError(result.over)
        return _Plan(native, face2d, frame, result, planned.notes, src, False, planned.source_seconds,
                     planned.trace_seconds)
    except DecalError:
        _unsuppress(design, suppressed)
        raise


def _commit(design, component, base_name: str, native_face, planned: _Plan, cfg: dict, source: dict,
            progress: Progress, name, occurrence: str, suppressed: list, started: float) -> dict:
    base = native_face.body
    frame, result, notes, reuse = planned.frame, planned.result, planned.notes, planned.reused
    cuts = result.pieces_count if frame.kind == 'cylinder' else 0
    chosen = bake_mod.choose_name(design, component, name or source_label(source), set(store.names(design)), cuts)
    report = bake_mod.bake(design, component, base, native_face, frame, result, cfg, chosen, progress)
    report['seconds'] = {'source': round(planned.source_seconds, 2), 'trace': round(planned.trace_seconds, 2),
                         **report['seconds']}
    report['trace_stages'] = {k: round(v, 2) for k, v in result.timings.items()}
    record = {
        'name': chosen,
        'settings': cfg,
        'source': dict(source),
        'face': dict(frame.record(), component=component.name, body=base_name, occurrence=occurrence),
        'inlay_body': report['inlay_body'],
        'stored_body': report['stored_body'],
        'pieces': report['pieces'],
        'points': report['points'],
        'made': datetime.datetime.now().isoformat(timespec='seconds'),
    }
    inlay = component.bRepBodies.itemByName(report['inlay_body'])
    if inlay is None:
        raise DecalError(f'The body {report["inlay_body"]} is missing after the bake.')
    store.save(design, inlay, record)
    if report['stored_body']:
        stored = component.bRepBodies.itemByName(report['stored_body'])
        if stored is None:
            raise DecalError(f'The body {report["stored_body"]} is missing after the bake.')
        store.mark_stored(stored, chosen, base_name)
    report.update(suppressed=suppressed, notes=notes + list(result.warnings), reused_plan=reuse,
                  total_seconds=round(time.perf_counter() - started, 2))
    log(f'decal {chosen}: {report["pieces"]} pieces, {report["points"]} points, {report["lumps"]} lumps, '
        f'{report["inlay_volume_mm3"]:.2f} of {report["expected_volume_mm3"]:.2f} mm3, '
        f'seconds {report["seconds"]}, total {report["total_seconds"]} s, suppressed {suppressed}')
    return report


def target_face(design, rec: dict):
    """The face a decal's record points at in the model as it is, for the rebake preview.

    Looks through the bodies that come from the record's body, including stored cut bodies.
    Returns the face (a proxy when the component sits in an occurrence), the clicked point in
    the same space and the inlay bodies of the live decals on that body, whose pockets the
    preview leaves out.
    """
    component = _component(design, rec['face']['component'])
    base_name = rec['face']['body']
    occ = _occurrence(design, component, rec['face'].get('occurrence', ''))
    ignore = inlay_bodies(component, live_decals(design, component, base_name))
    for i in range(component.bRepBodies.count):
        body = component.bRepBodies.item(i)
        if store.inlay_of(design, body) is not None or store.base_of(design, body) != base_name:
            continue
        found = face_mod.match_face(body, rec['face'], ignore)
        if found is None:
            continue
        click = face_mod.point(tuple(rec['face']['click']))
        if occ is not None:
            found = found.createForAssemblyContext(occ)
            if found is None or not click.transformBy(occ.transform2):
                raise DecalError(f'Fusion could not place the face of decal {rec["name"]} in {occ.fullPathName}.')
        return found, click, ignore
    raise DecalError(f'No body in {component.name} that comes from {base_name} has the face of decal '
                     f'{rec["name"]} through its clicked point.')
