# Decal add-in for Fusion

Superseded in part, 2026-11-04 (owner's second talk). The dialog, the button, the rebake
section, "Where it lives" and the rule that a new decal suppresses the earlier decals on a body
are replaced by `2026-11-04-image-text-decal-design.md`, which splits the add-in into Image
Decal and Text Decal. That spec also replaces the message boxes of the Errors section (messages
live in the dialog's readout; one box remains, for a failure inside the bake), the refusals of
more than one face, of art wider than the face and of a one-tone image (now taken, clipped or
warned), the click-centred art and quarter-turn rotation of pipeline item 4, the click-fixed
tangent point of cylinder rule 2, and the orange preview. Its section "Changes to the earlier
pipeline" lists every such change. The pipeline, the bake, the constants, the ceilings and the
core tests below otherwise still apply.

Owner 2026-11-04. A Fusion add-in that takes a PNG, an SVG or typed text and cuts it into a
face the user picks, as a flush 0.4 mm two-colour inlay. It does inside Fusion what the lid
decals of 2026-11-02 and 2026-11-03 took three Mac scripts and a run of MCP calls to do
(`tools/cane-weave/inlay_export.py`, `fusion_inlay.py`, `photo_inlay.py`). Those scripts stay
as history; new decals go through the add-in.

## Decisions (owner, 2026-11-04 talk)

1. Self-contained in Fusion. No Mac-side step and no library beyond Fusion's own Python
   (3.14, with zlib and a file dialog, without numpy or PIL; checked 2026-11-04).
2. The user picks the face. Flat faces and cylinders are accepted, because Fusion's emboss
   wraps a sketch onto those without stretching. Spheres and free-form faces are rejected by
   name.
3. Every failure names its cause in a message and leaves the design as it was.
4. Keep-outs can be turned off. When on, each round-hole size gets its own keep-out diameter,
   and every other cutout and the outer edge get one margin. All of them are user-set.
5. From the same talk, agreed as a list: a preview before the cut; width, offset, rotation
   and mirror controls; a printable minimum width for pieces and for the gaps between them;
   a progress bar with cancel; the settings stored with the decal and a rebake button; grey,
   alpha and scale handling for images; an add-in with a toolbar button, one undo step, and a
   plain-Python core that ctest can run without Fusion; typed text as a third source.
6. Future decals get simplified artwork (owner 2026-11-03). Outlines are simplified to
   0.08 mm, and the add-in counts points and pieces before it touches the timeline.
7. The cut body is stored as well as the inlay body, so unsuppressing the decal costs
   nothing (the plaster decal of 2026-11-03 froze Fusion on unsuppress while it rebuilt a
   38,562-line sketch and a live cut). A checkbox keeps the live cut instead, for a face that
   is still changing.

## Where it lives

| Item | Place |
|---|---|
| Add-in folder | `tools/decal/Decal/` (`Decal.py`, `Decal.manifest`, `resources/`) |
| Plain-Python core | `tools/decal/Decal/decal_core/` (`png.py`, `raster.py`, `trace.py`, `clip.py`, `plan.py`), stdlib only, no `adsk` import |
| Tests | `tools/decal/tests/`, run by ctest as `decal_core_test` with the Mac's `python3 -m unittest` |
| Log | `tools/decal/Decal/decal.log`, gitignored, one line per run and per failure with the traceback |
| In Fusion | Scripts and Add-Ins, Add-Ins tab, added once by its folder; or a symlink at `~/Library/Application Support/Autodesk/Autodesk Fusion 360/API/AddIns/Decal` so it loads at start |
| Button | DECAL under the Modify panel of the Solid tab, plus REBAKE DECAL next to it (owner looked under Modify, 2026-11-04; Create until then) |

## The dialog

| Input | Default | Notes |
|---|---|---|
| Face | none | one flat or cylindrical face; the point clicked sets the centre of the art |
| Source | Image | Image (PNG), SVG, or Text |
| File | none | file dialog; PNG or SVG by extension and by header |
| Text, font, bold, italic | empty, Fusion's default font | Text source only; fonts from Fusion's installed list |
| Trace | Dark | Dark (dark pixels become inlay) or Shading (relief lit from the upper right, the plaster photo's method); Image source only |
| Threshold | 128 of 255 for Dark; 0 for Shading | the Shading value is the shading level that counts as shadow |
| Invert | off | swaps which side becomes inlay |
| Width | face width less two margins | mm; height follows the source's aspect |
| Offset X, Y | 0, 0 | mm from the clicked point |
| Rotation | 0° | 0, 90, 180, 270 |
| Mirror | off | for a face that prints face down and is read from the other side |
| Minimum width | 0.8 mm | pieces and the gaps between them under this are removed; one 0.4 mm nozzle cannot print them |
| Minimum area | 0.5 mm² | loops under this are dropped |
| Keep-outs | on | off runs the art to every edge |
| Round holes, one row per size | hole diameter + 0.4 mm | the row shows the hole size and count, the field is the keep-out diameter centred on the hole |
| Margin | 0.2 mm | other cutouts, the recess and the outer edge |
| Depth | 0.4 mm | the user parameter `inlayDepth` when the design has one, else created |
| Colour | Black | Black or White; the inlay body gets the appearance `Decal inlay black` or `Decal inlay white`, created once in the design from Fusion's appearance library and reused by name, so recolouring it once recolours every decal (owner 2026-11-04, the decal must be visible in Fusion) |
| Store cut body | on | decision 7 |
| Readout | | pieces, points, dropped pieces, inlay area, estimated sketch time; red with the reason when a ceiling is passed, and OK is disabled |

The dialog redraws the preview after every change. The preview is custom graphics (filled
outlines on the face, keep-outs as rings), never a sketch, so it costs no timeline rebuild
and leaves nothing behind on Cancel. On a cylinder the preview is drawn wrapped.

## Constants in `Decal.py`

| Constant | Value | Why |
|---|---|---|
| Raster pitch | 0.1 mm (10 px/mm) | matches the lid decals; 0.08 mm simplify sits under it |
| Simplify | 0.08 mm | decision 6; a 0.4 mm nozzle shows nothing finer |
| Point ceiling | 10,000 | 30–45 k points took 10–27 min of sketch time on 2026-11-03; with sketch compute deferred the add-in draws 6,802 lines in 25 s (built 2026-11-04) |
| Piece ceiling | 1,000 | 565 pieces baked in 82 s end to end (2026-11-04); the flat cut of 565 profiles took 12 s |
| Time estimate | points / 5,000 per minute, plus 0.55 s per piece on a cylinder | measured 2026-11-04: 6,802 points in 78 s on a flat face; one emboss per piece at 0.29 s and 0.24 s to delete |
| Progress | stage boundaries and about every 2 % of each slow loop in the trace, every piece and every emboss in the bake | cancel checked at each step; the core takes a `progress(stage, fraction)` callback |

The constants live in `decal_core/plan.py`, the one place that computes the estimate; `Decal.py` reads them.

## One pipeline for three sources

Every source becomes a black-and-white raster at 0.1 mm, so keep-outs, minimum width,
tracing and simplifying are written once in `decal_core`.

1. **Image.** `png.py` decodes the PNG itself with zlib (bit depths 1 to 16, grey, RGB,
   palette, with or without alpha). Colour goes to grey by luma. Transparent pixels are not
   art. The image is box-filtered down (or up, with a warning when a source pixel is wider
   than 0.3 mm) to the chosen width at 10 px/mm. Dark mode thresholds the grey. Shading mode
   divides by a wide blur to remove the lighting and subtracts a 2.5 mm blur to leave the
   relief, as `photo_inlay.py` did with 80 px and 25 px at the same pitch.
2. **SVG.** Fusion imports the file into a scratch sketch on a scratch plane with
   `Sketch.importSVG`. The add-in reads every closed loop back as a polyline at 0.08 mm
   tolerance, scales the set to the chosen width, rasterises it even-odd, and removes the
   scratch sketch. Fusion's own SVG parser does the curve work.
3. **Text.** Fusion lays the text out with `SketchTexts.add` on the same scratch sketch, and
   the loops are read back the same way.
4. **Frame to face.** Rotation and mirror are applied to the raster, then the raster is laid on
   the face with its centre at the clicked point plus the offset. On a flat face the frame is
   the face's plane. On a cylinder the frame is the unrolled face, u along the circumference
   as arc length and v along the axis, with the tangent plane at the clicked point as the
   sketch plane.
5. **Keep-outs.** The face's outer loop is inset by the margin; inner loops are classed as
   round holes (one circular edge) or other cutouts; round holes get a circle of the row's
   diameter, other cutouts an outward offset of the margin. All of it is drawn into the
   raster as not-art.
6. **Minimum width.** Erosion and dilation by half the minimum width, both ways, through a
   two-pass chamfer distance transform, so a 1.35 M pixel face takes seconds in plain Python.
   Pieces that vanish are counted as dropped.
7. **Trace.** Border following gives outer loops counter-clockwise and holes clockwise, the
   polylines are simplified to 0.08 mm, loops under the minimum area go.
8. **Count.** Points and pieces against the ceilings; area; the time estimate. This fills the
   readout and gates OK.

## The bake (on OK, one undo step)

| Feature | Flat face | Cylinder |
|---|---|---|
| Sketch `<decal>` | on the face | on the tangent plane at the clicked point |
| Cut | one extrude `<decal> cut`, `inlayDepth` into the body, direction from the face's outward normal | one emboss per profile, `<decal> cut 1` to `<decal> cut N`, depth −`inlayDepth`, no tangent chain. Fusion's emboss takes several profiles only when they share the position along the axis and otherwise fails with "Input profiles are not coplanar" (measured 2026-11-04), so N features it is, 0.29 s each |
| Inlay body `<decal> inlay` | (a box over the art ∩ a copy of the body before the cut) − a copy of the body after it, temporary booleans, added in a base feature as one body of many lumps (1.6 s for 565 pieces) | a copy before − a copy after, the same way (0.09 s for 7 pieces) |
| Stored cut body `<decal> body` | a copy of the cut body in a base feature, then a Remove feature `<decal> remove` on the parametric body, then the cut and the sketch deleted | same; the tangent plane, its level plane and point sketch, the sketch and every emboss go into a temporary timeline group that one delete removes |
| Group `<decal>` | the three features that stay | same |

Profiles are matched to pieces by bounding box, four numbers within 0.01 mm, with the loop
count as a second check. Fusion's area call costs 54 ms per profile at every accuracy and its
bounding-box call 0.13 ms, so the matching of 578 profiles fell from 34 s to under 0.1 s. The
sketch draws with compute deferred and is solved once. A cylinder decal makes N + 7 features
with the stored cut body and N + 5 with a live cut; three stay in either case.

1. The inlay body gets the chosen `Decal inlay` appearance; the stored cut body keeps the
   face's own appearance. The preview draws in a fixed translucent orange, which reads on
   dark and light faces alike.
2. `<decal>` is the file's stem or the text, with ` 2`, ` 3` added when the name is taken,
   because feature names are unique per component and Fusion would otherwise rename silently.
3. Volume check before the group closes. The inlay body's volume must match the traced area
   times the depth within 0.1 %, and its lump count must equal the piece count, else the
   command aborts (lesson 11 of the 2026-11-02 builds). No lid-against-inlay intersection
   check; that one froze Fusion for 70 min.
4. The settings (every dialog value, the file path, the face's body name, plane or axis and
   clicked point) are stored as JSON in an attribute on the inlay body and listed in a design
   attribute, under the group `cubevox-decal`.
5. Other decals the add-in made on the same body are suppressed, not deleted. Hand-made
   groups (the 2026-11-02 and 2026-11-03 lid groups) are left alone.
6. Health of every new feature is read back; a warning or error is reported with the
   feature's name and message.

## Rebake

REBAKE DECAL lists the decals found in the design attributes. Picking one deletes its group,
re-finds the face on the stored body by plane or axis and the clicked point, and runs the
bake with the stored settings and the stored file. The dialog opens filled in, so a value can
be changed first. This is how a decal follows an upstream change to the face once its cut
body is stored.

## Errors

Every case below shows a message box naming the cause, writes the traceback to the log, and
ends with the design as it was before OK. The command runs as one Fusion command, so a
failure after features exist aborts the command and Fusion drops them.

| Case | Message names |
|---|---|
| No face, or more than one | what to pick |
| Face is not flat or cylindrical | the face's surface type |
| File missing, unreadable, or not what its extension says | the path and the header found |
| PNG interlaced, or a chunk the decoder does not handle | the PNG feature, and that a re-export from Preview fixes it |
| Image all one colour after the threshold | the threshold and the grey range found |
| SVG or text with no closed loop | the count of open curves found |
| Font not installed | the font name |
| Art wider than the face, or nothing left after keep-outs and minimum width | the widths, or the counts before and after |
| Over a ceiling | the count and the ceiling (also shown live, with OK disabled) |
| Cancel during trace or bake | nothing added; no message |
| A feature fails or comes out unhealthy | the feature name and Fusion's message |
| Volume or lump count off | expected and found |

## Cylinders

1. Emboss wraps the sketch round the face, so the art keeps its width along the arc. The
   art can be no wider than the face's arc length less two margins, and the readout says so.
2. The clicked point is the art's centre and the sketch plane's tangent point.
3. Printing. A flat face prints face down and the inlay comes out flush and clean. A curved
   face cannot, so the 0.4 mm inlay prints as layer steps and the second colour sits on wall
   lines. That looks right on a cylinder printed upright and poor on a gentle dome, which is
   one more reason domes are refused.

## Tests

1. `decal_core_test` under ctest, no Fusion. The test writes its own PNGs with zlib for every
   colour type and bit depth and checks the decoded pixels; an interlaced PNG is rejected by
   name. A 10 mm square traces to one loop of 100 mm² within 0.5 %; a ring gives an outer loop
   and a hole; a 0.6 mm bar is dropped and a 1.0 mm bar kept at the 0.8 mm minimum; a keep-out
   of 16 mm round a 7 mm hole removes a 16 mm disc of art; simplifying holds 0.08 mm; the
   ceilings trip at their values.
2. Fusion check, run by the lead through the MCP script runner on a scratch design with a flat
   plate with four hole sizes and a 40 mm cylinder. The command's core is callable without
   the dialog, with the same settings dict the dialog builds. The check reads back body count,
   inlay volume against area × depth, lump count against piece count, and feature health, and
   times the trace and the bake. Agents never call Fusion.
3. The owner's check is the dialog itself on the lid, with the cane proof PNG
   (`build/cane/proof.png`) and a short text.

## As built, 2026-11-04

Runs on the scratch design `decal-scratch` (a 150 × 90 × 5 mm plate with eleven holes of
four sizes and a window pocket, and a 40 mm cylinder), through `api.run_decal` from the
Fusion script runner. Every volume matched the drawn area × depth to the printed precision,
every inlay body had one lump per piece, and the timeline held only the group afterwards.

| Run | Pieces | Points | Total | Sketch | Profiles | Cut | Inlay | Store |
|---|---|---|---|---|---|---|---|---|
| Text "cubevox", flat, 60 mm | 7 | 570 | 2.8 s | 0.46 | 0.14 | 0.39 | 0.12 | 0.46 |
| Cane proof PNG, flat, full width, keep-outs on | 565 | 6,802 | 82 s | 25.4 | 33.9 (area call, since replaced) | 11.9 | 1.6 | 6.5 |
| Text "Adam", flat, 90 mm, black | 4 | 766 | 3.1 s | 0.61 | 0.04 (bounding box) | 0.61 | 0.17 | 0.64 |
| Text "cubevox", cylinder, 60 mm, white | 7 | 570 | 3.0 s | 0.50 | 0.03 | 1.47 (7 embosses) | 0.22 | 0.46 |
| SVG, flat, keep-outs off, live cut | 4 | 618 | 1.6 s | 0.49 | 0.04 | 0.52 | 0.14 | none |
| Rebake of "Adam" while suppressed | 4 | 766 | 3.5 s | 0.65 | 0.04 | 0.62 | 0.18 | 0.71 |
| Cane proof PNG again, after the matching fix | 565 | 6,802 | 52 s | 26.9 | 0.24 | 14.3 | 1.7 | 6.3 |
| Six error cases on the stored cylinder body (missing file, SVG named PNG, art wider than the face, empty text, unknown font, rotation 45°) | | | | each raised its named message and left groups, bodies and timeline unchanged | | | | |

1. The core gate `decal_core_test` runs 127 tests under ctest. The whole trace of a 150 ×
   90 mm face takes 0.4 s in dark mode and 2.3 s in shading mode on the Mac.
2. The inlay body carried `Decal inlay black` or `Decal inlay white`, created once in the
   design. The plate screenshot showed the black text plainly against the plate.
3. Each new decal on a body suppressed the add-in's earlier decals on it and cut into the
   base body again. The SVG run reported its one open path as left out.
4. The wrapped text on the cylinder read upright and unmirrored. At high zoom the letter
   edges show the 0.1 mm raster steps; a 0.4 mm nozzle cannot reproduce them.
5. Error cases headless: a missing file and a wrong colour raised their named errors with
   the design untouched. The first headless run also showed that `run_decal` suppressed the
   earlier decal on the body before reading the source, so an error left it suppressed;
   the order was changed to describe, read, trace, then suppress and bake.
6. The dialog itself has not been driven; the owner's first use is its check.

## Open

1. The point and piece ceilings are still the hand-build numbers. 565 pieces and 6,802
   points took 82 s before the matching fix, so both ceilings could rise; the owner's first
   real decals set them.
2. A failure inside the bake itself (after the source and the plan have passed) is undone by
   Fusion's command abort in the dialog. A headless caller of `api.run_decal` is left with
   the earlier decals suppressed and the features made so far; a script that raises out is
   rolled back whole by Fusion, a script that catches the error is not.
3. Filament colours (owner).
