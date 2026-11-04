"""Decal add-in for Fusion.

Cuts a PNG, an SVG or typed text into a picked face as a flush two-colour inlay. The one
command, Decal, sits in the Modify panel of the Solid tab (owner 2026-11-04). Its first input
picks New or an existing decal; an existing one fills the dialog from its record and OK makes
it again. Spec: docs/specs/2026-11-04-decal-addin-design.md. The plain-Python core is
decal_core; the Fusion steps are decal_fusion, whose api.run_decal and api.rebake are also the
headless entries.

Every handler runs inside a guard. A DecalError shows its text in a message box, any other
exception shows its type and message, and both go to decal.log with the traceback. In execute
both also set executeFailed, so Fusion drops every feature the command made.
"""

import html
import math
import os
import sys

import adsk.core
import adsk.fusion

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from decal_core import plan  # noqa: E402
from decal_core.errors import DecalError  # noqa: E402
from decal_fusion import LOG_PATH, Cancelled, Progress, log, log_failure  # noqa: E402
from decal_fusion import api, bake, store  # noqa: E402
from decal_fusion import face as face_mod  # noqa: E402

CMD_DECAL = 'cubevoxDecal'
CMD_NAME = 'Decal'
CMD_TOOLTIP = 'Cut a PNG, an SVG or text into a face as a flush inlay.'
RESOURCES = os.path.join(_HERE, 'resources')   # icons written by tools/decal/make_icons.py
WORKSPACE_ID = 'FusionSolidEnvironment'
PANEL_ID = 'SolidModifyPanel'
NEW = 'New'

FILL_RGB = (255, 120, 0)   # translucent orange reads on dark and light faces alike
FILL_OPACITY = 0.55
LINE_RGB = (200, 80, 0)
RING_RGB = (40, 110, 220)
FILL_LIFT_MM = 0.02        # the preview floats this far off the face so it never flickers
LINE_LIFT_MM = 0.03
WRAP_STEP_MM = 1.0         # longest preview segment along a cylinder's arc
FILL_BANDS_MM = (0.2, 0.4, 0.8, 1.6)
MAX_FILL_QUADS = 30_000
RING_SEGMENTS = 48

SOURCES = ('Image', 'SVG', 'Text')
TRACES = ('Dark', 'Shading')
ROTATIONS = ('0°', '90°', '180°', '270°')
THRESHOLD_DEFAULT = {'Dark': 128, 'Shading': 0}

# Inputs whose change leaves the traced pieces as they are, including the groups and the
# table, which may report a fold or a row change.

NO_RESULT = {'depth', 'store_cut', 'colour', 'readout', 'holes', 'art_group', 'place_group',
             'clean_group', 'keep_group', 'cut_group'}

COLOURS = ('Black', 'White')

_handlers = []     # (event, handler) for every connected handler, kept alive until stop()
_session = None    # the open Decal dialog


def _ui():
    return adsk.core.Application.get().userInterface


# --- Guards ------------------------------------------------------------------------------

def _tell(text: str, where: str, session) -> None:
    """A message box, shown once per distinct text from the preview and validate handlers,
    which Fusion fires again and again."""
    if session is not None and where in ('executePreview', 'validateInputs'):
        if session.last_told == text:
            return
        session.last_told = text
    _ui().messageBox(text, 'Decal')


def _guard(where: str, fn, args, session=None) -> None:
    try:
        fn(args)
    except DecalError as e:
        log(f'{where}: {e}')
        _tell(str(e), where, session)
    except Exception as e:
        log_failure(where, e)
        _tell(f'{type(e).__name__}: {e}\n\nThe traceback is in {LOG_PATH}.', where, session)


def _guard_execute(session, args) -> None:
    """execute's guard. Every failure sets executeFailed so Fusion aborts the command's
    transaction and drops every feature it made. A cancel shows nothing."""
    try:
        session.execute(args)
    except Cancelled:
        log('execute: cancelled by the user')
        args.executeFailed = True
        args.executeFailedMessage = ''
    except DecalError as e:
        log(f'execute: {e}')
        args.executeFailed = True
        args.executeFailedMessage = str(e)
        _ui().messageBox(str(e), 'Decal')
    except Exception as e:
        log_failure('execute', e)
        text = f'{type(e).__name__}: {e}\n\nThe traceback is in {LOG_PATH}.'
        args.executeFailed = True
        args.executeFailedMessage = text
        _ui().messageBox(text, 'Decal')


class _CreatedHandler(adsk.core.CommandCreatedEventHandler):
    def notify(self, args):
        _guard('commandCreated', _created, args)


class _InputChangedHandler(adsk.core.InputChangedEventHandler):
    def __init__(self, session):
        super().__init__()
        self.session = session

    def notify(self, args):
        _guard('inputChanged', self.session.input_changed, args, self.session)


class _ValidateHandler(adsk.core.ValidateInputsEventHandler):
    def __init__(self, session):
        super().__init__()
        self.session = session

    def notify(self, args):
        _guard('validateInputs', self.session.validate, args, self.session)


class _PreviewHandler(adsk.core.CommandEventHandler):
    def __init__(self, session):
        super().__init__()
        self.session = session

    def notify(self, args):
        _guard('executePreview', self.session.preview, args, self.session)


class _ExecuteHandler(adsk.core.CommandEventHandler):
    def __init__(self, session):
        super().__init__()
        self.session = session

    def notify(self, args):
        _guard_execute(self.session, args)


class _DestroyHandler(adsk.core.CommandEventHandler):
    def __init__(self, session):
        super().__init__()
        self.session = session

    def notify(self, args):
        _guard('destroy', self.session.destroy, args, self.session)


def _created(args) -> None:
    global _session
    session = _Session(args.command)
    _session = session
    session.connect()
    session.build()
    session.refresh()


# --- Preview geometry --------------------------------------------------------------------

def _split(a: float, b: float, wrap: bool) -> list:
    """Points from a to b, at most WRAP_STEP_MM apart on a cylinder."""
    n = max(1, math.ceil(abs(b - a) / WRAP_STEP_MM)) if wrap else 1
    return [a + (b - a) * k / n for k in range(n + 1)]


def _outline_strips(pieces: list, frame) -> tuple[list, list]:
    """Every loop of every piece as a closed line strip in root space."""
    wrap = frame.kind == 'cylinder'
    coords, lengths = [], []
    for piece in pieces:
        for loop in [piece.outer] + list(piece.holes):
            count = 0
            for (x0, y0), (x1, y1) in zip(loop, loop[1:] + loop[:1]):
                xs = _split(x0, x1, wrap)
                for k, x in enumerate(xs[:-1]):
                    y = y0 + (y1 - y0) * k / (len(xs) - 1)
                    coords.extend(frame.world(frame.model(x, y, LINE_LIFT_MM)))
                    count += 1
            coords.extend(frame.world(frame.model(loop[0][0], loop[0][1], LINE_LIFT_MM)))
            lengths.append(count + 1)
    return coords, lengths


def _bands(pieces: list) -> list:
    """Even-odd scanline runs of all loops as (y0, y1, [(xa, xb), ...]) bands.

    The band height doubles until the run count fits MAX_FILL_QUADS; an empty list means
    the fill is left out and only the outlines show.
    """
    edges = [(a, b) for p in pieces for loop in [p.outer] + list(p.holes)
             for a, b in zip(loop, loop[1:] + loop[:1]) if a[1] != b[1]]
    if not edges:
        return []
    ymin = min(min(a[1], b[1]) for a, b in edges)
    ymax = max(max(a[1], b[1]) for a, b in edges)
    for step in FILL_BANDS_MM:
        rows = max(1, math.ceil((ymax - ymin) / step))
        buckets = [[] for _ in range(rows)]
        for a, b in edges:
            lo, hi = min(a[1], b[1]), max(a[1], b[1])
            for r in range(max(0, int((lo - ymin) / step - 0.5)), min(rows - 1, int((hi - ymin) / step)) + 1):
                buckets[r].append((a, b))
        out, quads = [], 0
        for r in range(rows):
            yc = ymin + (r + 0.5) * step
            xs = sorted(a[0] + (yc - a[1]) * (b[0] - a[0]) / (b[1] - a[1])
                        for a, b in buckets[r] if (a[1] <= yc < b[1]) or (b[1] <= yc < a[1]))
            runs = list(zip(xs[0::2], xs[1::2]))
            quads += len(runs)
            out.append((yc - step / 2, yc + step / 2, runs))
        if quads <= MAX_FILL_QUADS:
            return out
    return []


def _fill_triangles(pieces: list, frame) -> list:
    """The bands as triangles in root space, wound to face out of the face."""
    wrap = frame.kind == 'cylinder'
    coords = []
    for y0, y1, runs in _bands(pieces):
        for xa, xb in runs:
            xs = _split(xa, xb, wrap)
            for s0, s1 in zip(xs, xs[1:]):
                p00 = frame.world(frame.model(s0, y0, FILL_LIFT_MM))
                p10 = frame.world(frame.model(s1, y0, FILL_LIFT_MM))
                p11 = frame.world(frame.model(s1, y1, FILL_LIFT_MM))
                p01 = frame.world(frame.model(s0, y1, FILL_LIFT_MM))
                coords.extend(p00 + p10 + p11 + p00 + p11 + p01)
    return coords


def _ring_strips(face2d, frame, settings: dict) -> tuple[list, list]:
    """A ring at each round hole's keep-out diameter."""
    if not settings['keepouts']:
        return [], []
    coords, lengths = [], []
    for x, y, d in face2d.holes:
        keep = settings['hole_keepouts'].get(round(d, 1), d + 2 * settings['margin_mm'])
        for k in range(RING_SEGMENTS + 1):
            t = 2 * math.pi * k / RING_SEGMENTS
            coords.extend(frame.world(frame.model(x + keep / 2 * math.cos(t), y + keep / 2 * math.sin(t),
                                                  LINE_LIFT_MM)))
        lengths.append(RING_SEGMENTS + 1)
    return coords, lengths


def _colour(rgb: tuple):
    return adsk.fusion.CustomGraphicsSolidColorEffect.create(adsk.core.Color.create(*rgb, 255))


# --- The dialog --------------------------------------------------------------------------

class _Session:
    """One open Decal dialog: its inputs, the described face, the loaded source, the cached plan
    and the preview graphics. rebake_name is the existing decal picked in the first input, or
    None for a new one."""

    def __init__(self, command):
        self.command = command
        self.app = adsk.core.Application.get()
        self.design = adsk.fusion.Design.cast(self.app.activeProduct)
        if self.design is None:
            raise DecalError('Decal works in a Fusion design, and the active document is not one.')
        self.i = {}
        self.events = []
        self.face = None
        self.click = None
        self.face_rec = None       # the picked decal's stored face, to recognise its face when re-selected
        self.decal_click = None    # the picked decal's clicked point, in the selection's space
        self.face2d = None
        self.frame = None
        self.face_id = None
        self.live = []
        self.hole_rows = []
        self.hole_gen = 0
        self.paths = {'png': None, 'svg': None}
        self.source = None
        self.source_id = None
        self.source_notes = []
        self.loops_pending = False
        self.result = None
        self.error = None
        self.version = 0
        self.preview_group = None
        self.drawn = -1
        self.geometry = None
        self.width_touched = False
        self.width_default_cm = None
        self.filling = False
        self.last_told = None
        self.rebake_name = None

    # Wiring

    def connect(self) -> None:
        cmd = self.command
        pairs = ((cmd.inputChanged, _InputChangedHandler(self)),
                 (cmd.validateInputs, _ValidateHandler(self)),
                 (cmd.executePreview, _PreviewHandler(self)),
                 (cmd.execute, _ExecuteHandler(self)),
                 (cmd.destroy, _DestroyHandler(self)))
        for event, handler in pairs:
            if not event.add(handler):
                raise DecalError('Fusion did not connect a handler of the decal dialog.')
            self.events.append((event, handler))
            _handlers.append((event, handler))

    def _dropdown(self, parent, input_id: str, name: str, items, selected: str):
        dd = parent.addDropDownCommandInput(input_id, name, adsk.core.DropDownStyles.TextListDropDownStyle)
        for item in items:
            dd.listItems.add(item, item == selected, '')
        self.i[input_id] = dd
        return dd

    def _mm_input(self, parent, input_id: str, name: str, mm: float):
        self.i[input_id] = parent.addValueInput(input_id, name, 'mm',
                                                adsk.core.ValueInput.createByString(f'{mm:g} mm'))

    def _check_input(self, parent, input_id: str, name: str, value: bool):
        self.i[input_id] = parent.addBoolValueInput(input_id, name, True, '', value)

    def build(self) -> None:
        inputs = self.command.commandInputs
        self._dropdown(inputs, 'decal', 'Decal', [NEW] + store.names(self.design), NEW)

        # No minimum: a picked decal's face is found from its record even when Fusion will not
        # show it as selected, and validate refuses OK while no face is described.

        sel = inputs.addSelectionInput('face', 'Face', 'Click the flat or cylindrical face at the centre of the art')
        sel.addSelectionFilter('PlanarFaces')
        sel.addSelectionFilter('CylindricalFaces')
        sel.setSelectionLimits(0, 1)
        self.i['face'] = sel

        art = inputs.addGroupCommandInput('art_group', 'Art').children
        self._dropdown(art, 'source', 'Source', SOURCES, 'Image')
        button = art.addBoolValueInput('file', 'File', False, '', False)
        button.text = 'Choose a file'
        self.i['file'] = button
        self.i['text'] = art.addStringValueInput('text', 'Text', '')
        fonts = list(self.app.fontNames)
        self._dropdown(art, 'font', 'Font', fonts, 'Arial' if 'Arial' in fonts else (fonts[0] if fonts else ''))
        self._check_input(art, 'bold', 'Bold', False)
        self._check_input(art, 'italic', 'Italic', False)
        self._dropdown(art, 'trace', 'Trace', TRACES, 'Dark')
        threshold = art.addIntegerSliderCommandInput('threshold', 'Threshold', 0, 255)
        threshold.valueOne = THRESHOLD_DEFAULT['Dark']
        self.i['threshold'] = threshold
        self._check_input(art, 'invert', 'Invert', False)

        place = inputs.addGroupCommandInput('place_group', 'Placement').children
        self._mm_input(place, 'width', 'Width', 10.0)
        self._mm_input(place, 'offset_x', 'Offset X', 0.0)
        self._mm_input(place, 'offset_y', 'Offset Y', 0.0)
        self._dropdown(place, 'rotation', 'Rotation', ROTATIONS, ROTATIONS[0])
        self._check_input(place, 'mirror', 'Mirror', False)

        clean = inputs.addGroupCommandInput('clean_group', 'Clean-up').children
        self._mm_input(clean, 'min_width', 'Minimum width', plan.DEFAULTS['min_width_mm'])
        self.i['min_area'] = clean.addFloatSpinnerCommandInput('min_area', 'Minimum area (mm²)', '', 0.0,
                                                               10000.0, 0.1, plan.DEFAULTS['min_area_mm2'])

        keep = inputs.addGroupCommandInput('keep_group', 'Keep-outs').children
        self._check_input(keep, 'keepouts', 'Keep-outs', plan.DEFAULTS['keepouts'])
        table = keep.addTableCommandInput('holes', 'Round holes', 2, '3:2')
        table.minimumVisibleRows = 1
        table.maximumVisibleRows = 6
        self.i['holes'] = table
        self._mm_input(keep, 'margin', 'Margin', plan.DEFAULTS['margin_mm'])

        cut = inputs.addGroupCommandInput('cut_group', 'Cut').children
        param = self.design.userParameters.itemByName(bake.DEPTH_PARAMETER)
        self._mm_input(cut, 'depth', 'Depth', param.value * 10.0 if param is not None else plan.DEFAULTS['depth_mm'])
        self._dropdown(cut, 'colour', 'Colour', COLOURS, api.FUSION_DEFAULTS['colour'].title())
        self._check_input(cut, 'store_cut', 'Store cut body', plan.DEFAULTS['store_cut'])

        self.i['readout'] = inputs.addTextBoxCommandInput('readout', 'Readout', '', 5, True)

    # Reading the inputs

    def _choice(self, input_id: str):
        item = self.i[input_id].selectedItem
        return None if item is None else item.name

    def _mm(self, input_id: str) -> float:
        return self.i[input_id].value * 10.0

    def settings(self) -> dict:
        """The settings dict for api.run_decal, keyed as decal_core.plan.DEFAULTS."""
        return {
            'mode': 'shading' if self._choice('trace') == 'Shading' else 'dark',
            'threshold': int(self.i['threshold'].valueOne),
            'invert': bool(self.i['invert'].value),
            'width_mm': self._mm('width') if self.width_touched else None,
            'offset_x': self._mm('offset_x'),
            'offset_y': self._mm('offset_y'),
            'rotation': ROTATIONS.index(self._choice('rotation')) * 90,
            'mirror': bool(self.i['mirror'].value),
            'min_width_mm': self._mm('min_width'),
            'min_area_mm2': float(self.i['min_area'].value),
            'keepouts': bool(self.i['keepouts'].value),
            'hole_keepouts': {d: field.value * 10.0 for d, field in self.hole_rows},
            'margin_mm': self._mm('margin'),
            'depth_mm': self._mm('depth'),
            'store_cut': bool(self.i['store_cut'].value),
            'colour': self._choice('colour').lower(),
        }

    def source_spec(self):
        """The source dict for api.run_decal, or None while it is incomplete."""
        chosen = self._choice('source')
        if chosen == 'Text':
            text = ' '.join(self.i['text'].value.split())
            if not text:
                return None
            return {'kind': 'text', 'text': text, 'font': self._choice('font') or '',
                    'bold': bool(self.i['bold'].value), 'italic': bool(self.i['italic'].value)}
        kind = 'svg' if chosen == 'SVG' else 'png'
        path = self.paths[kind]
        return {'kind': kind, 'path': path} if path else None

    def _problem(self):
        """Why the numbers cannot be used, or None."""
        if self.width_touched and self._mm('width') <= 0:
            return 'The width must be more than 0 mm.'
        if self._mm('depth') <= 0:
            return 'The depth must be more than 0 mm.'
        if min(self._mm('min_width'), self._mm('margin'), float(self.i['min_area'].value)) < 0:
            return 'The minimum width, minimum area and margin cannot be negative.'
        return None

    def _quietly(self, fn) -> None:
        """Change inputs from code without reacting to the inputChanged events it fires."""
        self.filling = True
        try:
            fn()
        finally:
            self.filling = False

    # Events

    def input_changed(self, args) -> None:
        if self.filling:
            return
        changed = args.input.id
        if changed == 'face':
            self._take_face()
        elif changed == 'decal':
            self.take_decal()
        elif changed == 'file':
            self._choose_file()
        elif changed in ('source', 'text', 'font', 'bold', 'italic'):
            self._take_source()
        elif changed == 'trace':
            def reset():
                self.i['threshold'].valueOne = THRESHOLD_DEFAULT[self._choice('trace')]
            self._quietly(reset)
        elif changed == 'width':
            self.width_touched = (self.width_default_cm is None
                                  or abs(self.i['width'].value - self.width_default_cm) > 1e-9)
        elif changed == 'margin':
            self._default_width()
        if changed not in NO_RESULT and not changed.startswith('holelabel_'):
            self.recompute()
        self.refresh()

    def validate(self, args) -> None:
        args.areInputsValid = False
        if self.face2d is None or self.source_spec() is None or self._problem() is not None:
            return
        if self.loops_pending:
            # The preview reads SVG and text loops; execute reads them itself if OK comes first.
            args.areInputsValid = True
            return
        args.areInputsValid = self.result is not None and self.error is None and self.result.over is None

    def preview(self, args) -> None:
        args.isValidResult = False
        if self.loops_pending and self.face2d is not None:
            # An SVG or text needs a scratch sketch, and Fusion clears the selection when the
            # model changes inside inputChanged. So the loops are read here once per change of
            # the source, and the plan is computed right after.
            self.loops_pending = False
            spec = self.source_spec()
            if spec is not None:
                try:
                    self.source, self.source_notes = api.load_source(self.design, spec, Progress(None, pump=False))
                except DecalError as e:
                    self.source, self.error = None, str(e)
                    self.refresh()
                    raise
                self.recompute()
                self.refresh()
        self._draw()

    def execute(self, args) -> None:
        self._clear_preview()
        ui = self.app.userInterface
        dialog = ui.createProgressDialog()
        dialog.isCancelButtonShown = True
        dialog.isBackgroundTranslucent = False
        dialog.show('Decal', 'Starting  %p%', 0, 100, 0)
        try:
            report = self._run(Progress(dialog))
        finally:
            dialog.hide()
        if report['warnings']:
            ui.messageBox('The decal was made, and Fusion reports warnings:\n' + '\n'.join(report['warnings']),
                          'Decal')

    def _run(self, progress: Progress) -> dict:
        settings = self.settings()
        spec = self.source_spec()
        if spec is None:
            raise DecalError('No source is chosen.')
        if self.face is None or self.click is None:
            raise DecalError('No face is picked.')
        cached = None
        if self.result is not None and self.face2d is not None and self.source_id is not None:
            cached = {'face2d': self.face2d, 'source_key': self.source_id, 'settings': settings,
                      'result': self.result, 'notes': self.source_notes}
        if self.rebake_name is None:
            return api.run_decal(self.design, self.face, self.click, settings, spec, progress, cached=cached)
        return api.rebake(self.design, self.rebake_name, settings, spec, progress, cached=cached,
                          face=self.face, click_point=self.click)

    def destroy(self, args) -> None:
        global _session
        try:
            self._clear_preview()
        finally:
            for pair in self.events:
                if pair in _handlers:
                    _handlers.remove(pair)
            self.events = []
            if _session is self:
                _session = None

    # The face

    def _take_face(self) -> None:
        """A face picked by hand, or Fusion echoing the face the dialog selected for a picked
        decal. On the picked decal's own face the decal's stored clicked point is kept, so
        selecting that face again does not move the decal."""
        sel = self.i['face']
        self.face = self.click = self.face2d = self.frame = None
        self.live = []
        key = None
        if sel.selectionCount == 1:
            picked = sel.selection(0)
            face = adsk.fusion.BRepFace.cast(picked.entity)
            if face is None:
                raise DecalError('The selection is not a face.')
            body = (face.nativeObject or face).body
            inlay = store.inlay_of(self.design, body)
            if inlay is not None:
                self._quietly(sel.clearSelection)
                self.recompute()
                self.refresh()
                raise DecalError(f'That face is on {body.name}, the inlay body of decal {inlay}. '
                                 'Pick a face on the part.')
            component = body.parentComponent
            base = store.base_of(self.design, body)
            live = api.live_decals(self.design, component, base)
            fill = api.inlay_bodies(component, live)
            click = picked.point
            if (self.face_rec is not None and self.decal_click is not None
                    and component.name == self.face_rec['component']
                    and face_mod.holds(face.nativeObject or face, self.face_rec, fill)):
                click = self.decal_click
            try:
                face2d, frame = face_mod.describe(face, click, fill)
            except DecalError:
                self._quietly(sel.clearSelection)
                self.face_id = None
                self._rebuild_holes({})
                self.recompute()
                self.refresh()
                raise
            self.face, self.click, self.face2d, self.frame = face, click, face2d, frame
            self.live = [rec for rec in live if rec['name'] != self.rebake_name]
            key = face_mod.face_key(face2d, frame, base)
        if key != self.face_id:
            self.face_id = key
            self._rebuild_holes({})
        self._default_width()

    def take_decal(self) -> None:
        """New keeps the dialog as it is for a new decal. A decal's name fills the dialog from its
        record and finds its face, which OK then makes the decal on again."""
        name = self._choice('decal')
        if not name or name == NEW:
            self.rebake_name = self.face_rec = self.decal_click = None
            self.live = self._live_for(self.face)
            return
        rec = store.load(self.design, name)
        if rec is None:
            raise DecalError(f'The design has no record for decal {name}.')
        self.rebake_name, self.face_rec, self.decal_click = name, rec['face'], None
        self.face = self.click = self.face2d = self.frame = None
        self.live = []
        self._quietly(lambda: self._fill(rec))
        try:
            face, click, ignore = api.target_face(self.design, rec)
        except DecalError as e:
            self._quietly(self.i['face'].clearSelection)
            self.face_id = None
            self._rebuild_holes(rec['settings']['hole_keepouts'])
            self._take_source()
            self.recompute()
            self.refresh()
            raise DecalError(f'{e} Pick the face for decal {name} by hand.') from e
        self.face2d, self.frame = face_mod.describe(face, click, ignore)
        self.face, self.click, self.decal_click = face, click, click
        self._quietly(lambda: self._show_face(face))
        self.live = [r for r in self._live_for(face) if r['name'] != name]
        self.face_id = face_mod.face_key(self.face2d, self.frame, rec['face']['body'])
        self._rebuild_holes(rec['settings']['hole_keepouts'])
        self._default_width()
        self._take_source()

    def _live_for(self, face) -> list:
        """The add-in's live decals on the body a face comes from."""
        if face is None:
            return []
        body = (face.nativeObject or face).body
        return api.live_decals(self.design, body.parentComponent, store.base_of(self.design, body))

    def _show_face(self, face) -> None:
        """Show the decal's face as the selection. The face is already described, so a face
        Fusion will not select still bakes; the log says so."""
        sel = self.i['face']
        sel.clearSelection()
        if not sel.addSelection(face):
            log(f'Fusion did not show the face of decal {self.rebake_name} as selected')

    def _select(self, input_id: str, name: str) -> None:
        items = self.i[input_id].listItems
        for k in range(items.count):
            if items.item(k).name == name:
                items.item(k).isSelected = True
                return
        raise DecalError(f'The decal\'s stored {input_id} "{name}" is not offered in this Fusion.')

    def _fill(self, rec: dict) -> None:
        s, src = rec['settings'], rec['source']
        self._select('source', {'png': 'Image', 'svg': 'SVG', 'text': 'Text'}[src['kind']])
        if src['kind'] in ('png', 'svg'):
            self.paths[src['kind']] = src['path']
        else:
            self.i['text'].value = src['text']
            if src.get('font'):
                self._select('font', src['font'])
            self.i['bold'].value = bool(src.get('bold'))
            self.i['italic'].value = bool(src.get('italic'))
        self._select('trace', 'Shading' if s['mode'] == 'shading' else 'Dark')
        self.i['threshold'].valueOne = int(s['threshold'])
        self.i['invert'].value = bool(s['invert'])
        self.width_touched = s['width_mm'] is not None
        if self.width_touched:
            self.i['width'].value = s['width_mm'] / 10.0
        self.i['offset_x'].value = s['offset_x'] / 10.0
        self.i['offset_y'].value = s['offset_y'] / 10.0
        self._select('rotation', ROTATIONS[(int(s['rotation']) // 90) % 4])
        self.i['mirror'].value = bool(s['mirror'])
        self.i['min_width'].value = s['min_width_mm'] / 10.0
        self.i['min_area'].value = float(s['min_area_mm2'])
        self.i['keepouts'].value = bool(s['keepouts'])
        self.i['margin'].value = s['margin_mm'] / 10.0
        self.i['depth'].value = s['depth_mm'] / 10.0
        self._select('colour', s.get('colour', api.FUSION_DEFAULTS['colour']).title())
        self.i['store_cut'].value = bool(s['store_cut'])
        self._show_file()

    def _rebuild_holes(self, values: dict) -> None:
        """One row per round-hole size of the face: the size and count, and the keep-out."""
        def rebuild():
            table = self.i['holes']
            while table.rowCount > 0:
                if not table.deleteRow(0):
                    raise DecalError('Fusion did not clear the round-hole rows.')
            self.hole_rows = []
            self.hole_gen += 1
            if self.face2d is None:
                return
            sizes = {}
            for _, _, d in self.face2d.holes:
                sizes[round(d, 1)] = sizes.get(round(d, 1), 0) + 1
            parent = table.commandInputs
            for row, d in enumerate(sorted(sizes)):
                label = parent.addStringValueInput(f'holelabel_{self.hole_gen}_{row}', '',
                                                   f'Ø {d:.1f} mm × {sizes[d]}')
                label.isReadOnly = True
                keep = values.get(d, d + 2 * plan.DEFAULTS['margin_mm'])
                field = parent.addValueInput(f'hole_{self.hole_gen}_{row}', '', 'mm',
                                             adsk.core.ValueInput.createByString(f'{keep:g} mm'))
                if not (table.addCommandInput(label, row, 0) and table.addCommandInput(field, row, 1)):
                    raise DecalError('Fusion did not add a round-hole row.')
                self.hole_rows.append((d, field))
        self._quietly(rebuild)

    def _default_width(self) -> None:
        if self.face2d is None or self.width_touched:
            return
        width_mm = self.face2d.width_mm - 2 * self._mm('margin')
        if width_mm <= 0:
            return
        self.width_default_cm = width_mm / 10.0

        def set_width():
            self.i['width'].value = self.width_default_cm
        self._quietly(set_width)

    # The source

    def _show_file(self) -> None:
        kind = 'svg' if self._choice('source') == 'SVG' else 'png'
        path = self.paths[kind]
        button = self.i['file']
        button.text = os.path.basename(path) if path else 'Choose a file'
        button.tooltip = path or 'Choose a PNG or SVG file'

    def _choose_file(self) -> None:
        kind = 'svg' if self._choice('source') == 'SVG' else 'png'
        dialog = self.app.userInterface.createFileDialog()
        dialog.isMultiSelectEnabled = False
        dialog.title = 'Choose an SVG drawing' if kind == 'svg' else 'Choose a PNG image'
        dialog.filter = 'SVG drawings (*.svg)' if kind == 'svg' else 'PNG images (*.png)'
        if dialog.showOpen() != adsk.core.DialogResults.DialogOK:
            return
        self.paths[kind] = dialog.filename
        self._take_source()

    def _take_source(self) -> None:
        """Load the source when its content changed: a PNG at once, an SVG or text in the
        next preview."""
        self._show_file()
        spec = self.source_spec()
        key = api.source_key(spec) if spec is not None else None
        if key == self.source_id and (self.source is not None or self.loops_pending or key is None):
            return
        self.source, self.source_notes, self.loops_pending = None, [], False
        self.source_id = key
        if spec is None:
            return
        if spec['kind'] == 'png':
            self.source_id = None
            self.source, self.source_notes = api.load_source(self.design, spec, Progress(None, pump=False))
            self.source_id = key
        else:
            self.loops_pending = True

    # The plan

    def recompute(self) -> None:
        """Run decal_core's plan for the current face, source and settings. Called after an
        input that shapes the pieces changed, and once after the preview reads SVG or text
        loops; never on a plain preview."""
        self.version += 1
        self.result = None
        self.error = self._problem()
        if self.error is None and self.face2d is not None and self.source is not None:
            try:
                self.result = plan.compute(self.source, self.face2d, self.settings())
            except DecalError as e:
                # A plan that fails while the user drags a value goes to the readout and the
                # log; a message box per step of a slider would bury the dialog.
                self.error = str(e)
                log(f'plan: {e}')

    def refresh(self) -> None:
        """Visibility of the inputs and the readout text."""
        chosen = self._choice('source')
        image, text = chosen == 'Image', chosen == 'Text'
        self.i['file'].isVisible = not text
        for input_id in ('text', 'font', 'bold', 'italic'):
            self.i[input_id].isVisible = text
        self.i['trace'].isVisible = image
        self.i['threshold'].isVisible = image
        self.i['holes'].isVisible = bool(self.i['keepouts'].value) and bool(self.hole_rows)
        self.i['readout'].formattedText = '<br/>'.join(self._readout_lines())

    def _readout_lines(self) -> list:
        esc = html.escape
        red = '<span style="color:#c00000">{}</span>'
        lines = []
        spec = self.source_spec()
        if self.rebake_name is not None:
            lines.append(esc(f'OK makes decal {self.rebake_name} again with these values.'))
        if self.face2d is None:
            lines.append('Pick a face.')
        elif spec is None:
            lines.append('Type the text.' if self._choice('source') == 'Text' else 'Choose a file.')
        elif self.loops_pending:
            lines.append('Reading the shapes.')
        elif self.error:
            lines.append(red.format(esc(self.error)))
        elif self.result is not None:
            r = self.result
            lines.append(f'Pieces {r.pieces_count:,}, points {r.points:,}')
            lines.append(f'Dropped {r.dropped_thin} under the minimum width, {r.dropped_small} under the minimum area')
            lines.append(f'Inlay area {r.area_mm2:,.1f} mm², sketch time about {r.est_minutes:.1f} min')
            if r.over:
                lines.append(red.format(esc(r.over)))
            lines.extend(esc(w) for w in r.warnings)
        lines.extend(esc(n) for n in self.source_notes)
        if self.live:
            names = ', '.join(rec['name'] for rec in self.live)
            lines.append(esc(f'On OK the add-in suppresses its decals on this body: {names}.'))
        return lines

    # The preview

    def _clear_preview(self) -> None:
        if self.preview_group is not None and self.preview_group.isValid:
            if not self.preview_group.deleteMe():
                raise DecalError('Fusion did not remove the decal preview.')
        self.preview_group = None
        self.drawn = -1

    def _draw(self) -> None:
        """Draw the cached plan as custom graphics, once per plan. Nothing is drawn into the
        model, so the preview costs no timeline compute and leaves nothing on Cancel."""
        if self.result is None or self.frame is None:
            self._clear_preview()
            return
        if self.preview_group is not None and self.preview_group.isValid and self.drawn == self.version:
            return
        self._clear_preview()
        if self.geometry is None or self.geometry[0] != self.version:
            settings = self.settings()
            pieces = self.result.pieces
            self.geometry = (self.version, _fill_triangles(pieces, self.frame),
                             _outline_strips(pieces, self.frame), _ring_strips(self.face2d, self.frame, settings))
        _, fill, (line_coords, line_lengths), (ring_coords, ring_lengths) = self.geometry
        group = self.design.rootComponent.customGraphicsGroups.add()
        if group is None:
            raise DecalError('Fusion did not create the preview graphics group.')
        self.preview_group = group
        if fill:
            mesh = group.addMesh(adsk.fusion.CustomGraphicsCoordinates.create(fill), [], [], [])
            mesh.color = _colour(FILL_RGB)
            if not mesh.setOpacity(FILL_OPACITY, True):
                raise DecalError('Fusion refused the opacity of the decal preview.')
            mesh.cullMode = adsk.fusion.CustomGraphicsCullModes.CustomGraphicsCullNone
            mesh.depthPriority = 1
            mesh.isSelectable = False
        for coords, lengths, rgb in ((line_coords, line_lengths, LINE_RGB), (ring_coords, ring_lengths, RING_RGB)):
            if coords:
                lines = group.addLines(adsk.fusion.CustomGraphicsCoordinates.create(coords), [], True, lengths)
                lines.color = _colour(rgb)
                lines.weight = 2.0
                lines.depthPriority = 2
                lines.isSelectable = False
        self.drawn = self.version


# --- Add-in entry ------------------------------------------------------------------------

def _add_button(ui) -> None:
    definitions = ui.commandDefinitions
    old = definitions.itemById(CMD_DECAL)
    if old is not None and not old.deleteMe():
        raise DecalError(f'Fusion did not remove the old {CMD_NAME} button.')
    definition = definitions.addButtonDefinition(CMD_DECAL, CMD_NAME, CMD_TOOLTIP, RESOURCES)
    handler = _CreatedHandler()
    if not definition.commandCreated.add(handler):
        raise DecalError(f'Fusion did not connect the {CMD_NAME} button.')
    _handlers.append((definition.commandCreated, handler))
    workspace = ui.workspaces.itemById(WORKSPACE_ID)
    panel = workspace.toolbarPanels.itemById(PANEL_ID) if workspace is not None else None
    if panel is None:
        raise DecalError(f'Fusion has no {PANEL_ID} panel in {WORKSPACE_ID} for the {CMD_NAME} button.')
    control = panel.controls.itemById(CMD_DECAL)
    if control is not None and not control.deleteMe():
        raise DecalError(f'Fusion did not remove the old {CMD_NAME} control.')
    if panel.controls.addCommand(definition) is None:
        raise DecalError(f'Fusion did not add the {CMD_NAME} button to the {PANEL_ID} panel.')


def _start(_args) -> None:
    _add_button(_ui())
    log('add-in started')


def _finish(_args) -> None:
    ui = _ui()
    if _session is not None:
        _session._clear_preview()
    while _handlers:
        event, handler = _handlers.pop()
        if not event.remove(handler):
            raise DecalError('Fusion did not disconnect a decal handler.')
    workspace = ui.workspaces.itemById(WORKSPACE_ID)
    panel = workspace.toolbarPanels.itemById(PANEL_ID) if workspace is not None else None
    control = panel.controls.itemById(CMD_DECAL) if panel is not None else None
    if control is not None and not control.deleteMe():
        raise DecalError(f'Fusion did not remove the control {CMD_DECAL}.')
    definition = ui.commandDefinitions.itemById(CMD_DECAL)
    if definition is not None and not definition.deleteMe():
        raise DecalError(f'Fusion did not remove the command {CMD_DECAL}.')

    # Fusion keeps imported modules between runs; dropping them makes a restart load edits.

    for module in [m for m in sys.modules if m.split('.')[0] in ('decal_core', 'decal_fusion')]:
        del sys.modules[module]
    if _HERE in sys.path:
        sys.path.remove(_HERE)
    log('add-in stopped')


def run(context):
    _guard('run', _start, context)


def stop(context):
    _guard('stop', _finish, context)
