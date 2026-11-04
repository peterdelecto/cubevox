# Decal add-in: agent brief

Spec: `docs/specs/2026-11-04-decal-addin-design.md`. Read it first, whole. This brief fixes the
contract between the two halves so they can be written at the same time.

## Rules for both agents

1. Python 3.14 (Fusion's own and the Mac's `/opt/homebrew/bin/python3`). Standard library
   only in `decal_core`. No numpy, PIL, scipy, cv2 anywhere.
2. Never call Fusion. No `mcp__fusion__*` tools, no AppleScript, no launching Fusion. The lead
   runs the Fusion checks.
3. No macOS Developer Tools. Verify with `python3 -I` and `ctest --test-dir build`.
4. 4-space indent, type hints, short docstrings that state present behaviour. Comments say
   why, not history. No colon-hinged sentences in prose.
5. Errors: `DecalError(message)` carries the full user-facing text from the spec's Errors
   table. Never catch an exception without re-raising or turning it into a `DecalError` that
   names the cause. No bare `except:`.
6. Edit only under `tools/decal/` plus the one `add_test` line in `CMakeLists.txt` (agent A).
   Nothing else in the repo. No commits.
7. Reports: write `build/audit/decal-core.md` (agent A) or `build/audit/decal-fusion.md`
   (agent B) with what was built, what was checked and the measured times. Reply with a few
   lines only.

## Layout

```
tools/decal/
  Decal/                      the add-in folder (Fusion needs folder name == main file stem)
    Decal.py                  add-in entry, the two commands, dialog, preview, progress, log
    Decal.manifest
    decal_core/               plain Python, no `adsk` import           (agent A)
      __init__.py errors.py png.py raster.py clip.py trace.py plan.py
    decal_fusion/             Fusion-side helpers, imports adsk        (agent B)
      __init__.py face.py bake.py store.py api.py
  tests/                      unittest files for decal_core            (agent A)
```

`decal.log` is written next to `Decal.py` and is gitignored (`tools/decal/Decal/decal.log`;
agent A adds that line to `.gitignore`).

## decal_core contract (agent A writes it, agent B codes against it)

All lengths in mm. The mask frame is image-like: pixel `(col, row)` has its centre at
`u = (col + 0.5) * PITCH_MM`, `v = (row + 0.5) * PITCH_MM`, `v` growing downward.

```python
# errors.py
class DecalError(Exception): ...            # str(e) is the message shown to the user

# png.py
def read(path: str) -> tuple[int, int, bytearray, bytearray | None]
    # (width, height, grey, alpha); row-major, 0..255. Luma for colour (0.299, 0.587, 0.114).
    # 16-bit takes the high byte. Palette with tRNS gives alpha. Bit depths 1, 2, 4, 8, 16.
    # DecalError (naming the path and the feature) for: not a PNG, interlaced (Adam7),
    # unknown critical chunk, CRC or zlib error, truncated data.

# raster.py
PITCH_MM = 0.1
class Mask:
    w: int; h: int; bits: bytearray     # len w*h, values 0 or 1; 1 is art
    warnings: list[str]
def from_grey(w, h, grey, alpha, width_mm, mode, threshold, invert) -> Mask
    # Box-filter resample to round(width_mm / PITCH_MM) columns, rows by aspect.
    # mode 'dark': art = grey <= threshold. mode 'shading': flat = g / blur(g, 8 mm);
    # relief = (flat - blur(flat, 2.5 mm)) * 230; art = relief <= -threshold (photo_inlay.py's
    # method at the same pitch; blurs are box blurs, three passes). invert flips art.
    # Alpha < 128 is never art, applied after invert. A source pixel wider than 0.3 mm on the
    # face adds a warning string.
def from_loops(loops: list[list[tuple[float, float]]], width_mm) -> Mask
    # Loops in any units, closed (last point != first). Scaled so the bbox width is width_mm,
    # filled even-odd by scanline at PITCH_MM.
def rotate(mask, quarter_turns: int) -> Mask     # clockwise on screen
def mirror(mask) -> Mask                          # left-right
def distance(mask, to_art: bool) -> array.array  # 'f', per pixel, mm, exact Euclidean
    # (two-pass 1D transform per row then per column); to_art=False gives distance to the
    # nearest non-art pixel.
def open_close(mask, min_width_mm) -> tuple[Mask, int]
    # Opening by min_width/2 removes pieces narrower than min_width; closing by the same
    # fills gaps narrower than min_width. Second value is pieces lost (connected components,
    # 8-connected, before minus after).
def intersect(a, b) -> Mask
def count_pieces(mask) -> int

# clip.py  (everything already in mask-frame coordinates)
def fill(w, h, loop) -> Mask                                   # even-odd, one loop
def allowed(w, h, outer, inners, margin_mm) -> Mask            # inside outer, outside every
                                                               # inner, then eroded by margin
def clear_circles(mask, circles: list[tuple[float, float, float]]) -> Mask  # (u, v, diameter)

# trace.py
class Piece:
    outer: list[tuple[float, float]]        # counter-clockwise in (u, v right/up terms:
    holes: list[list[tuple[float, float]]]  # see plan for the flip), holes clockwise
    area_mm2: float
def loops(mask, simplify_mm, min_area_mm2) -> tuple[list[Piece], int]
    # Crack following along pixel edges so areas are exact, Douglas-Peucker (closed) at
    # simplify_mm, loops under min_area dropped (count returned), holes assigned to the
    # outer that contains them.
def points(pieces) -> int

# plan.py
CEILING_POINTS = 10_000
CEILING_PIECES = 1_000
POINTS_PER_MINUTE = 2_700
DEFAULTS = dict(mode='dark', threshold=128, invert=False, width_mm=None, offset_x=0.0,
                offset_y=0.0, rotation=0, mirror=False, min_width_mm=0.8, min_area_mm2=0.5,
                keepouts=True, hole_keepouts={}, margin_mm=0.2, depth_mm=0.4, store_cut=True)
class Face2D:            # built by the Fusion side, face coordinates in mm, y up
    kind: str            # 'flat' | 'cylinder'
    outer: list[tuple[float, float]]
    inners: list[list[tuple[float, float]]]   # cutouts that are not round holes
    holes: list[tuple[float, float, float]]   # round holes (x, y, diameter); not in inners
    centre: tuple[float, float]               # the clicked point
    width_mm: float      # flat: bbox width of outer; cylinder: the face's arc length
class Source:
    kind: str            # 'image' | 'loops'
    w: int; h: int; grey: bytearray; alpha: bytearray | None     # image
    loops: list[list[tuple[float, float]]]                       # loops
class Result:
    pieces: list[Piece]  # in face coordinates
    points: int; pieces_count: int; dropped_thin: int; dropped_small: int
    area_mm2: float; est_minutes: float
    over: str | None     # reason text when a ceiling is passed, else None
    warnings: list[str]
def compute(source: Source, face: Face2D, settings: dict) -> Result
```

`compute` does, in order: mask from the source at `width_mm` (default `face.width_mm -
2 * margin_mm`); `rotate`; `mirror`; the frame-to-face map `x = cx + offset_x + (u - W/2)`,
`y = cy + offset_y - (v - H/2)` where `W, H` are the mask size in mm; keep-outs (when on:
`allowed(outer, inners, margin)` with the loops mapped face-to-frame, then `clear_circles`
for each hole with `hole_keepouts.get(round(d, 1), d + 2 * margin)`; when off: `allowed(outer,
inners + hole outlines, 0)`, because art over a hole is air); `open_close(min_width)`;
`loops(simplify 0.08, min_area)`; pieces mapped to face coordinates; counts, area, the
estimate `points / POINTS_PER_MINUTE`, and `over` when a ceiling is passed. `DecalError`
when the art is wider than the face or nothing is left (messages per the spec's table).

## Agent A (decal_core and tests)

1. Write `decal_core` to the contract. Speed target for the whole `compute` on a 150 × 90 mm
   flat face with a 1000 × 600 photo and four hole sizes: under 60 s on the Mac; measure
   and report. The distance transform and the resample are the hot loops; use `bytearray`,
   `array`, `memoryview` and per-row slicing, never per-pixel Python objects.
2. Tests in `tools/decal/tests/test_*.py`, `unittest`, run with
   `python3 -I -m unittest discover -s tools/decal/tests -t tools/decal/Decal`
   (the `-t` makes `decal_core` importable). Cases from the spec's Tests section: PNGs the test
   writes itself with `zlib` for every colour type and bit depth, an interlaced one rejected
   by name; a 10 mm square to one loop of 100 mm² within 0.5 %; a ring to an outer and a
   hole; a 0.6 mm bar dropped and a 1.0 mm bar kept at 0.8 mm; a 16 mm keep-out round a
   7 mm hole; simplify holding 0.08 mm; both ceilings tripping; `compute` end to end on a
   synthetic face.
3. One `add_test(NAME decal_core_test COMMAND python3 -I -m unittest discover -s
   ${CMAKE_SOURCE_DIR}/tools/decal/tests -t ${CMAKE_SOURCE_DIR}/tools/decal/Decal)` in
   `CMakeLists.txt` next to the other python tests (around line 206). Reconfigure with
   `cmake -S . -B build` and run `ctest --test-dir build -R decal_core_test`. Gate on
   "100% tests passed".

## Agent B (the add-in)

Facts checked in Fusion on 2026-11-04: Python 3.14.7; `numpy`, `PIL`, `cv2`, `scipy` absent;
`zlib`, `struct`, `tkinter` present; `ui.createFileDialog` present; `Sketch.importSVG` present;
`EmbossFeatures.createInput` present and `EmbossFeatureInput` has `profiles`, `inputFaces`,
`depth`, `isTangentChain`, `horizontalDistance`, `verticalDistance`, `rotationAngle`,
`targetBaseFeature`; `TemporaryBRepManager.booleanOperation` present; the user's add-ins
folder is `~/Library/Application Support/Autodesk/Autodesk Fusion 360/API/AddIns/`.

Read `tools/cane-weave/fusion_inlay.py` for the proven API patterns (sketch on a face,
`modelToSketchSpace`, profiles by area and centroid, `TemporaryBRepManager` slab route,
`baseFeatures.add` with `startEdit`/`finishEdit`, timeline groups). Lessons from those builds:

1. Points made with a guessed sketch-plane z land off the plane; set sketch-space z to 0.
2. Fusion keeps each disjoint lump as its own body unless the body is added whole as a
   temporary body in a base feature; the inlay must be one body of many lumps.
3. A sketch profile may ignore projected loops as islands; pick cut profiles by area and
   centroid against the pieces, never by count or order.
4. A cut that removes whole bodies leaves empty bodies; filter on `faces.count > 0`.
5. A script that raises is rolled back whole. In a command, set `args.executeFailed = True`
   and `args.executeFailedMessage` so Fusion drops every feature the command made.
6. Editing an extrude in place fails; delete and recreate.
7. Feature names are unique per component; Fusion silently appends ` (1)`. Choose the
   `<decal>` name before creating anything, adding ` 2`, ` 3` while a feature, body or
   group of that name exists.
8. The inlay body comes from temporary booleans, never from a second extrude of the
   profiles (40 min for 97 pieces) and never from a lid-against-inlay intersection check
   (70 min frozen). Inlay = (box over the art's bbox ∩ copy of the body before the cut)
   − copy of the body after the cut, on a flat face; before − after on a cylinder. Check
   `lumps.count == pieces` and volume within 0.1 % of `area × depth`.
9. Heavy features block the UI; call `adsk.doEvents()` between pieces and poll the progress
   dialog's `wasCancelled`.

Deliver:

1. `Decal.manifest` (type add-in, Fusion 360, `editEnabled` true, runOnStartup true).
2. `Decal.py`: `run(context)` adds DECAL and REBAKE DECAL buttons to the Solid tab's Create
   panel with an empty resource folder (the repo ignores `*.png`); `stop(context)` removes
   them and every handler. Handlers for `commandCreated`, `inputChanged`, `validateInputs`,
   `executePreview`, `execute`, `destroy`. The dialog per the spec's table. The round-hole
   rows are built after the face is selected, one per distinct diameter rounded to 0.1 mm.
   The readout text input shows pieces, points, dropped, area and the time estimate; when
   `Result.over` is set the text names the reason and `validateInputs` returns false. Every
   handler body is wrapped so that a `DecalError` shows `ui.messageBox(str(e))`, any other
   exception shows the exception type and message with the traceback in `decal.log`, and in
   `execute` both set `executeFailed`.
3. Preview in `executePreview` with a `CustomGraphicsGroup` on the root component: filled
   triangles (ear-clipped pieces) or line strips of every piece in a fixed colour and the
   keep-outs as rings, drawn in model space through the frame; on a cylinder wrapped by
   angle. Set `args.isValidResult = False` so nothing is kept. `compute` runs only in
   `inputChanged` when an input that affects the result changed, and its `Result` is cached
   on the command for preview and execute.
4. `decal_fusion/face.py`: `describe(face, click_point) -> (Face2D, Frame)`. Flat: origin at
   the click, x and y from the plane's u and v directions, outer and inner loops from the
   face's loops through `edge.evaluator.getStrokes` at 0.02 mm, a loop with one circle edge
   or arcs on one centre is a round hole. Cylinder: axis and radius from the face geometry,
   x = arc length from the click, y = axial, `width_mm` = the face's arc extent, and the
   tangent plane at the click. Reject every other surface type with the spec's message.
5. `decal_fusion/bake.py`: `bake(design, component, body, face, frame, result, settings,
   name, progress) -> dict`. Sketch of closed line loops (3D points from frame coordinates,
   then `modelToSketchSpace`), the cut (extrude on flat, emboss on cylinder), the inlay
   base feature, the stored cut body (base feature copy, `removeFeatures.add` on the
   parametric body, delete the sketch and the cut) when `store_cut`, the timeline group,
   the health read-back, the volume and lump check. Returns body count, inlay volume,
   lump count, feature names and seconds per stage.
6. `decal_fusion/store.py`: settings JSON in an attribute group `cubevox-decal` on the inlay
   body (name `settings`) and a list of decal names in a design attribute (`index`).
7. `decal_fusion/api.py`: `run_decal(design, face, click_point, settings, source, progress
   = None) -> dict` where `source` is `{'kind': 'png', 'path': ...}`, `{'kind': 'svg', 'path':
   ...}` or `{'kind': 'text', 'text': ..., 'font': ..., 'bold': ..., 'italic': ...}`. SVG and
   text go through a scratch sketch on a scratch construction plane (`importSVG`,
   `sketchTexts.add`), closed loops read back with `getStrokes` at 0.08 mm, then the scratch
   features deleted before the bake. `execute` calls this; so does the lead's MCP check,
   with a `settings` dict exactly as the dialog builds it. Also `suppress_others(design,
   body, name)` for the add-in's own earlier decals on that body, and `rebake(design, name)`.
8. Since you cannot run Fusion, check what you can: `python3 -I -m py_compile` on every
   file, a stub `adsk` package under `build/audit/adsk_stub/` only if you want to exercise
   control flow, and a careful read of each API call against the API reference
   (`help.autodesk.com/view/fusion360/ENU/?guid=...` through WebFetch). List every call you
   could not confirm in the report.
