# Image Decal and Text Decal add-ins for Fusion

Owner 2026-11-04, second talk. Two Fusion add-ins, each one button on the face of the Solid
tab's Modify panel. **Image Decal** takes a PNG or an SVG. **Text Decal** takes typed words. Both
cut the art 0.4 mm into one flat or cylindrical face the user picks and leave a flush two-colour
inlay.

This spec is written as the user's walk through each add-in, one step at a time. Each step says
what the user sees, what the user wants at that moment, what the add-in does about it, and what
happens when it goes wrong. The audit of that walk is `docs/audits/2026-11-04-decal-ux-audit.md`.
Its findings are folded in here and marked "(A5)" for the lead's walk and "(B22)" for the cold
read.

The raster pipeline at 0.1 mm, the keep-out drawing, the minimum width, the trace and simplify,
the bake, the constants, the ceilings and the core tests of `2026-11-04-decal-addin-design.md`
stay as written there, except where the section Changes to the earlier pipeline says otherwise.
This spec replaces that spec's "Where it lives", "The dialog", "Rebake" and button sections,
its rule that a new decal suppresses the earlier decals on the same body, its message boxes,
and splits the one add-in in two.

## Owner's direction, 2026-11-04 second talk

1. The first dialog was one long drawer that ran off the bottom of the screen.
2. Text had no way to choose a font, a size or a place. Text becomes its own add-in, made
   properly.
3. The user must see the decal on the face before it is cut, and must be able to drag or place
   it and size it on the model.
4. The spec designs from the user's point of view, step by step, anticipating the user's intent
   and goal at each step, for both add-ins. Then the spec is audited for experience and intent.

## Rules the walk gives

Every step below obeys these. The audit checks each step against them.

1. **Short dialog.** The dialog is 420 px wide. At open it shows at most fifteen rows, counting
   every row Fusion draws (group headers, the thumbnail and the readout each count one), and
   stands under 600 px (B1). One group is open at a time, so opening a group closes the others,
   and the tallest state of each dialog is named under its table (B2). Everything a first decal
   does not need sits in a collapsed group, and the group's name lists what is inside (B4).
2. **The model is the display.** Within a quarter of a second of any change something moves on
   the face. Move and turn are final at once. Changes that need a new trace show the old shapes
   scaled or greyed, the readout says "Updating…", and the clean result follows within the
   performance budget (B14, contradiction 5). Numbers in the dialog mirror the model.
3. **Hands on the model.** Place, turn and size are done by dragging handles on the face. The
   same values can be typed, and typing moves the handles. Mirror is a tick, since it is rare
   (A12, contradiction 10).
4. **Good first guess.** Once the face and the art are chosen the decal is already on the
   face, centred, upright as the user is looking at it, at a sensible size, black, 0.4 mm
   deep. For an image that is as large as fits inside the face's outline, or as large as its
   pixels allow. For words it is capitals a fifth of the face's shorter side (contradiction 6).
   Nothing has to be entered before something appears.
5. **Nothing until OK.** Before OK the design is untouched. Cancel or Esc, at any moment
   including during the bake, leaves every decal as it was before OK (B37). One undo takes back
   everything OK did (contradiction 11).
6. **Every refusal says what to change, with a number.** A problem shows the moment it arises,
   in the readout at the foot of the dialog, naming what happened and the change that fixes it
   (B39). OK is disabled only while a red line stands or nothing has been given yet (B18).
   Orange lines warn and let OK through.
7. **Coming back is the same dialog.** A decal made earlier opens in the dialog it was made
   with, every value and every handle where the user left them.
8. **Plain names.** Rows and messages use the user's words (Width, Height, Colour, Ink, Keep
   clear of holes, Smallest line), never the mechanism (threshold, erosion, emboss, points)
   (B6).

## The two add-ins at a glance

| | Image Decal | Text Decal |
|---|---|---|
| Button | Solid tab, Modify panel face, "Image Decal", picture icon | Solid tab, Modify panel face, "Text Decal", letter icon |
| Takes | one PNG or SVG file | typed words, one line or several |
| Own controls | which tones become inlay, width | font, bold and italic, Align, height, letter spacing, along an arc round a hole |
| Shared controls | face, place on the model, keep-outs, depth, colour, name, freeze, preview, readout, edit and delete | the same |
| Makes | one timeline group with the decal's name | one timeline group with the decal's name |

## Where it lives

| Item | Place |
|---|---|
| Image Decal add-in | `tools/decal/ImageDecal/` (`ImageDecal.py`, `ImageDecal.manifest`, `resources/` with the icon set) |
| Text Decal add-in | `tools/decal/TextDecal/` (`TextDecal.py`, `TextDecal.manifest`, `resources/`) |
| Shared, plain Python | `tools/decal/decal_core/` as today, plus `place.py` (face frame, fit, triad arithmetic, cylinder arc length, edge and hole distances) |
| Shared, Fusion side | `tools/decal/decal_fusion/` as today (`face`, `bake`, `store`, `api`), plus `dialog.py` (the rows both dialogs share), `preview.py` (the drawn preview) and `worker.py` (the decode-and-trace thread and its custom event) |
| Each add-in's first lines | put `tools/decal` on `sys.path`, resolved through the symlink with `os.path.realpath`, then import the shared packages |
| Tests | `tools/decal/tests/`, ctest `decal_core_test`, no Fusion |
| In Fusion | two symlinks in `~/Library/Application Support/Autodesk/Autodesk Fusion 360/API/AddIns/`, `ImageDecal` and `TextDecal`. Both load at start. Both buttons are promoted onto the Modify panel's face (`CommandControl.isPromoted`), not left in its dropdown (B30) |
| Settings kept between runs | `tools/decal/.settings.json`, gitignored. Last folder (first run, the home folder), last words, last font and style, the five recent fonts, last height, last colour, last depth. Written on OK and on Cancel alike (B36) |
| Thumbnails | `tools/decal/.cache/`, gitignored, one PNG per trace, the old one deleted |
| Log | `tools/decal/decal.log`, gitignored |

The one add-in built on 2026-11-04 (`tools/decal/Decal/`) becomes `ImageDecal/`. Its text
source is removed and lives on in Text Decal. The two shared packages move up one folder.

## The readout

Both dialogs end in one read-only text box the full width of the dialog, four lines, no label
(B3, contradiction 2). Every message is at most 70 characters so it fits on its line at 420 px.

| Line | Holds | Example |
|---|---|---|
| 1 | the face, and the decals already on it | `Flat face 147 × 87 mm, 11 holes · 2 decals on it` |
| 2 | the place. The art's box, its position, its distance from the two nearest face edges, and the nearest hole's clearance (A2, B12) | `40 × 25 mm at 12.0, −30.5 · 10.2 from left, 9.8 from top · hole 5.2 away` |
| 3 | the count and the cost | `565 pieces · 112 mm² · bake about 50 s · Preview the cut is below` |
| 4 | the message. Red refusals first, then orange warnings, then plain notes; when more than one stands the line ends with `+2 more` and the text box's tooltip lists them all | `Drag the dot to move, the ring to turn, the arrow to size` |

Before art is loaded line 2 is empty and line 4 carries the hint of the moment ("Choose an
image", "Type the words", "Click a face to see it there"). The drag hint stands on line 4 until
the first drag (A13). The piece count shows the points only when a ceiling is within 20 %
(B6).

## Image Decal, the walk

### Step 0. Before the dialog

**Sees** the body on screen. Most of the time a face is already selected, because Fusion users
click the thing and then the command. The Modify panel's face shows Image Decal with a picture
icon (B30).

**Wants** to put this picture on this face.

**Add-in does**
1. Opens a dialog 420 px wide (`setDialogInitialSize`), never taller than the rows it shows.
2. Takes a selected flat or cylindrical face as the face.
3. With nothing selected, opens in face-pick mode. Only flat faces and cylinders light up
   under the cursor (selection filters `PlanarFaces` and `CylindricalFaces`).

**Goes wrong**
1. The selected face is a sphere, a torus or a free-form face. The dialog opens with Face
   empty and line 4 says in red "Selected face is a torus. Pick a flat face or a cylinder."
2. More than one face was selected. The first flat or cylindrical one is taken and line 4 says
   so in orange.

### Step 1. The face

**Sees** a row **Face** at the top, and the face highlighted once picked.

**Wants** that one. The click is rough. The user does not expect it to fix the decal's place
for good.

**Add-in does**
1. One face, limit one. A second click replaces the first. A new face keeps every value except
   the place, and the decal is centred again on the new face. The same holds in Edit, so a
   decal can be moved to another face (B32).
2. Line 1 describes the face and counts the decals on it.
3. When the body already carries decals of either kind, a row **Edit** appears under Face, a
   dropdown with New and every decal's name (B27). Decals of the other kind are listed greyed
   with the add-in that edits them. On a body without decals the row is not there.
4. A click on the inlay body of a decal picks the body face under it, keeps New, lists that
   decal first in Edit, and line 4 says "You clicked the cane decal. Edit → cane to change
   it." A full-face decal covers most of the lid, so a click on it must still start a new
   decal (A5, B26).
5. When a decal on this body was cut before the face last changed, line 4 says so in orange,
   "cane was cut before the 7 mm hole moved. Edit → cane, OK re-cuts." (A6; how the add-in
   knows is in Edit and storage).

**Goes wrong** in no way the filter lets through. Clearing the face greys the Place group and
empties the preview. Image and Colour stay live, so an image can be chosen first (A4,
contradiction 3).

### Step 2. The image

**Sees** a row **Image** with a button "Choose…". After the choice, a thumbnail 90 px wide under
it, and the file name with its pixel size or, for an SVG, its drawn size in mm.

**Wants** the right file, and to know what of it will be cut.

**Add-in does**
1. The file dialog opens in the last folder and lists PNG, SVG and JPEG. A JPEG is let through
   the filter so the readout can say "JPEG is not taken. Export it as PNG from Preview." (B29).
2. The decode and the first trace run on the worker thread. Line 4 says "Reading logo.png…"
   first, then "Updating…", so a big photo never freezes Fusion with nothing said (B8). A PNG
   over 25 million pixels is refused with its size and the fix, "scale it under 5000 px".
3. The decal appears on the face at once at the default place and size (step 3). With no face
   picked yet, the thumbnail still appears and line 4 says "Click a face to see it there" (A4).
4. The default width is the fitted width, or the width at which one source pixel is 0.3 mm
   when that is smaller, so a small logo does not open blown up and blurred. Line 4 says
   which, "90 mm keeps pixels under 0.3 mm. Fit to face for larger." (A1).
5. The thumbnail is the trace, black pieces on white, not the source file. It shows what will
   be cut, so the tone changes of step 4 show there too. An SVG gets the same thumbnail from
   its loops.
6. Transparent pixels are not art, and colour goes to grey. In an SVG every closed shape is
   art whatever its fill colour, counted even-odd. The row's tooltip says all three.

**Goes wrong**
1. The file is missing, is not a PNG or SVG inside, is interlaced, or cannot be read. Line 4
   names it with the fix, "logo.png is interlaced. Open it in Preview, export as PNG." An image
   loaded before stays loaded and on the face, and the line is then orange, not red, so OK
   stays open for the decal that is there (B38).
2. The image is one tone after tracing. Orange, naming the fix by result, "All one tone.
   Choose Light areas." when the art is lighter than the cut, or "slide Ink to more" (B13).
3. An SVG with open paths. Orange, "3 open paths left out. Close them in the drawing."

### Step 3. Where and how big

**Sees** the decal on the face, centred, upright as seen from the current camera, as large as
fits inside the face's outline (B33). On it a triad with a dot at the centre that drags it
anywhere on the face, two arrows along the face that drag it in one direction, and a ring that
turns it. At the decal's right edge a second arrow, the width handle. In the dialog an open
group **Place** with the triad's rows X, Y and Angle, then **Width** with its arrow, then a row
of three buttons, **Centre**, **Fit to face** and **Place by click**.

**Wants** to move it here, make it this big, turn it. Then "exactly centred" or "exactly 40 mm
wide", by typing. Sometimes "put it right there", by clicking.

**Add-in does**
1. The triad (`TriadCommandInput`) is made once, at the face centre in the face frame, z
   along the outward normal, and is never re-created on a flat face, so its X, Y and Angle
   rows read the decal's position from the face centre and its turn, not the last drag (B11).
   Shown are the planar move, the x and y moves and the z rotation. All scaling and both
   flips are hidden (`hideAllScaling`), so the triad draws no Scale row (contradiction 9).
2. Width is a distance row with its own handle (`DistanceValueCommandInput.setManipulator`),
   an arrow at the decal's right edge pointing along x. Dragging it reads in mm, which is what
   the user thinks in, and no scale factor appears anywhere (B11). Height follows Width and
   shows as its own row with its own handle only while Keep proportions is off (A3).
3. Move and turn redraw the preview from the traced pieces without a new trace, so they
   follow the mouse. Width changes the raster, so the drawn pieces rescale at once and the
   clean trace replaces them when the thread finishes (section The preview).
4. X and Y are mm from the face centre, the centre of the face's bounding box in the face
   frame. 0, 0 is centred. Angle is any angle. A dropdown **Measure from** in the group Depth
   and print limits changes what X and Y count from, the face centre or any of its four
   corners, for "10 mm in from the top-left corner" (B12).
5. Typing any number moves the handles, and dragging a handle changes the numbers. The dialog
   holds one state, the decal's centre, angle, width, height and mirror.
6. Centre puts the decal at 0, 0. Fit to face sets the fitted size again at the current angle.
   Place by click arms one click. The next click on the face centres the decal there and the
   dialog returns to normal (B10). It is a selection input with the face filter that takes one
   point and is cleared at once, so the clicked point comes from `Selection.point`.
7. The fit is the largest width whose box lies inside the face's outline less the margin,
   found on the face's raster mask, so a round or notched face fits as well as a rectangle
   (B33).
8. The face frame. x runs along the face's longest straight edge, or along the component's X
   on a face with no straight edge. y is normal × x. When that y points down the screen at
   open, the frame is turned 180° so the decal reads upright from where the user is looking.
   On a cylinder x runs round the circumference as arc length and y along the axis, with the
   origin at mid-length under the first click, and y pointing up the screen (B33).
9. On a cylinder the triad lies in the tangent plane at the decal centre and is re-seated
   there after each drag. A drag of dx along its x moves the decal dx round the surface as arc
   length, not as a chord, so a drag can go past a quarter turn (B31). Because re-seating makes
   the triad's rows read the last drag, the Place group on a cylinder adds two rows of its
   own, **Round** and **Along**, in mm from the origin, and those are the truth.

**Goes wrong**
1. Part of the decal lies off the face or in a keep-out. It is clipped there in the preview
   and line 4 says in orange "12 pieces off the face. Centre, or move 4 mm right." with the
   shortest move that brings the box inside (B39). OK stays enabled.
2. The decal is scaled so small that pieces fall under the smallest line. They vanish from the
   preview and line 4 says in orange "8 pieces too thin at this size. Widen to 45 mm.", the
   width that brings the thinnest of them to 0.8 mm (B39).
3. Nothing of the decal is left on the face. Red, OK disabled, "Nothing left to cut. Press
   Centre." or "Nothing left to cut. Widen to 45 mm."
4. Art wider than a cylinder's circumference less two margins. Red, "Width 130 mm, the
   cylinder takes 125 mm. Narrow it."

### Step 4. What counts as ink

**Sees** a collapsed group **Tones (ink, dark or light, photo relief)**, shown for a PNG only.
Inside, **Inlay is** Dark areas | Light areas | Photo relief, and an **Ink** slider 0 to 255 with
"less" at its left end and "more" at its right (B6). The thumbnail in step 2 changes as the
slider moves.

**Wants** more ink or less ink, the other way round, or for a photo "make it a relief like the
plaster one".

**Add-in does**
1. Dark areas is the default at 128. Light areas is the old Invert, named by its result. Photo
   relief is the plaster photo method and its slider is the shading level.
2. Each slider step re-traces in the thread (0.4 s on a full lid face in Dark, 2.3 s in Photo
   relief). The thumbnail updates first because it is small, then the face.

**Goes wrong** when the result is one tone. Orange, with the fix by name (step 2).

### Step 5. Keep-outs

**Sees** a collapsed group with a tick in its header, **Keep clear of holes and edges** ☑.
Inside, **Margin** 0.2 mm and a table of the face's round holes by size, one row each with its
count and its keep-out diameter (hole + 0.4 mm). While the dialog is open the keep-outs show on
the face as a red tint over the art.

**Wants** usually nothing, since the tick is already on. Sometimes "run it to the edge"
(untick) or "more room round the big holes" (one number).

**Add-in does** everything live. The header tick turns all keep-outs off without opening the
group. The red tint belongs to the preview and leaves nothing behind.

**Goes wrong** when a keep-out diameter is set smaller than its hole. It is set back to the
hole size with an orange note.

### Step 6. Depth, colour and the rest

**Sees** a row **Colour** with two swatches, Black and White, always visible. Two collapsed
groups. **Depth and print limits** holds Depth 0.4 mm (the design's `inlayDepth` when it has
one), Smallest line 0.8 mm, Smallest speck 0.5 mm², Keep proportions, Mirror and Measure from
(B4, B6). **Name, freeze and preview** holds Name, Freeze the cut and the Preview the cut button.

**Wants** black on this white lid or the reverse, in one click. The rest almost never.

**Add-in does**
1. The preview is drawn in the chosen colour and the inlay body gets the `Decal inlay black`
   or `white` appearance. A depth under 0.2 mm or over the body's thickness at the face is
   refused in red, "Depth 1.2 mm, the wall is 1.0 mm. Use 0.8 mm or less."
2. Name defaults to the file's stem or the words, and is what the timeline group, the Edit
   row and every message call the decal (B7).
3. **Freeze the cut** is the earlier spec's stored cut body under the name of its effect. Its
   tooltip says "Opens fast. A later change to the face needs a re-cut (Edit → OK)." (B5). It
   defaults on only when the bake estimate is over 10 s. A small decal stays a live cut that
   follows the face on its own, so the stale case of Edit and storage arises only for big
   decals (B23).

### Step 7. Seeing it before it is cut

**Sees**, from the moment the image is loaded, the decal on the face in its colour, every piece
as it will be cut, each piece edged with a thin line in the opposite tone so white pieces read
on a light grey body and black pieces on a dark one (B17), a thin frame round the whole image,
and the keep-outs tinted red. While a new trace runs the old pieces stay and turn grey and
line 4 says "Updating… 0.8 s". The button **Preview the cut** runs the real cut and shows the
actual recess and inlay body in the design, with the progress bar. The dialog stays open
afterwards.

**Wants** "is that what I get?" answered without committing. For a big decal, "cut it now so OK
is quick".

**Add-in does**
1. The drawn preview is custom graphics (`CustomGraphicsGroup`, filled meshes and edge lines),
   90 % opaque in the inlay colour, lifted 0.02 mm off the face so it does not flicker against
   it. On a cylinder it is wrapped. It costs no timeline change and vanishes on Cancel.
2. Preview the cut runs the bake as Fusion's command preview with `isValidResult` true, so OK
   afterwards is instant. Its progress bar has Cancel, which returns to the drawn preview
   (B34). Line 3 carries its cost. Any change after it drops back to the drawn preview and
   line 4 says "Cut preview dropped. OK cuts again, about 50 s." (B34).

**Goes wrong** when the cut preview fails. Line 4 names the feature and Fusion's message in
red, the drawn preview returns, and nothing stays in the design.

### Step 8. The readout

**Sees** the four lines of section The readout.

**Wants** to know the cost before OK, and why something is missing.

**Add-in does** the counts from the plan. Over a ceiling (10,000 points, 1,000 pieces) line 4
is red with the count, the limit and the rows that lower it, "1,240 pieces, limit 1,000.
Smallest speck 1.5 mm² leaves 940." The number comes from one extra plan run in the thread at
the next step of that row (B9).

### Step 9. OK

**Sees** a progress bar with the stage name and Cancel. Then the dialog closes, one timeline
group with the decal's name appears, the inlay body shows in its colour, and Fusion's status
line says "Image Decal: cane, 565 pieces in 52 s" or, for a frozen cut, "Image Decal: cane
cut and frozen. Edits to the lid need Edit → OK to show." (B23).

**Wants** to get on with it, and to be able to take it back.

**Add-in does** the bake of the earlier spec as one undo step. Earlier decals on the same body
stay as they are, and the new one cuts the body as it stands (section Several decals on one
body, A7). Cancel or Esc during the bake, including the delete that starts an edit's run,
leaves every decal as it was before OK, because the whole run is one command that Fusion
aborts (B37, contradiction 18).

**Goes wrong** when anything inside the bake fails. The command aborts, Fusion drops the
features made so far, and a message box names the feature and Fusion's message. That box is
the one message box the add-ins show (contradiction 19).

### Step 10. Coming back to it

**Sees** Image Decal again, the same face clicked, and the **Edit** row listing the decals on
that body. Picking one fills every row, puts the handles where the decal is and draws the decal
as the preview. A **Delete** button joins the Place button row.

**Wants** "nudge it 2 mm", "swap the file", "the hole moved, cut it again", "take this one off".

**Add-in does**
1. Edit loads the stored settings (every value, the file, the frame) from the design
   attributes the earlier spec defined, and checks at once that the file and the font of this
   decal and of every decal above it in the stack still exist, naming any that is missing in
   red before OK (B40).
2. OK deletes the old group and bakes again under the same name, then re-cuts every decal
   made after it on that body, in order, from their stored settings and files. Line 3 says so
   before OK, "re-cuts 2 decals, about 55 s" (section Several decals on one body).
3. Delete hides the decal in the preview and line 4 says "OK deletes cane and re-cuts 1 decal
   above it, about 3 s." Nothing happens until OK, and one undo brings it back (B28). Delete
   works on a decal of either kind from either dialog (B27).
4. A group deleted by hand in the timeline is noticed at the next open, and line 4 says what
   to do (section Edit and storage).

**Goes wrong** when the stored file is gone. Red, naming the path. The user chooses another
file and the rest stays.

## Image Decal dialog

| Row | Open at start | Kind | Default |
|---|---|---|---|
| Face | yes | one flat or cylindrical face | preselection |
| Edit | only when the body carries decals | dropdown, New + every decal's name | New |
| Image | yes | button, then the file name | last folder |
| thumbnail | once a file is loaded | 90 px wide | |
| Colour | yes | two swatches | last used, first run Black |
| Place ▾ | open | group | |
| X, Y, Angle | | the triad's rows, named by Fusion | 0, 0, 0° |
| Round, Along | cylinders only | mm | 0, 0 |
| Width | | mm, with its arrow | fitted, or 0.3 mm per source pixel |
| Height | only while Keep proportions is off | mm, with its arrow | follows Width |
| Centre · Fit to face · Place by click · Delete | | button row; Delete only in Edit | |
| Tones (ink, dark or light, photo relief) ▸ | collapsed, PNG only | group | |
| Inlay is | | Dark areas / Light areas / Photo relief | Dark areas |
| Ink | | slider 0–255, less → more | 128 (Photo relief 0) |
| Keep clear of holes and edges ☑ ▸ | collapsed, tick in header | group | on |
| Margin | | mm | 0.2 |
| Round holes | | table, one row per size | hole + 0.4 mm |
| Depth and print limits ▸ | collapsed | group | |
| Depth | | mm | `inlayDepth` or 0.4 |
| Smallest line | | mm | 0.8 |
| Smallest speck | | mm² | 0.5 |
| Keep proportions | | tick | on |
| Mirror | | tick | off |
| Measure from | | Face centre / four corners | Face centre |
| Name, freeze and preview ▸ | collapsed | group | |
| Name | | text | file stem |
| Freeze the cut | | tick | on when the bake is over 10 s |
| Preview the cut | | button | |
| Readout | yes | four lines, full width | |

Rows at open with a PNG loaded, counting every row Fusion draws, are Face, Image, thumbnail,
Colour, the Place header, X, Y, Angle, Width, the button row, and the four group headers,
fourteen, plus the readout, fifteen. At 28 px a row with 53 px for the thumbnail, 70 px for
the readout and 75 px for the title and the OK bar, that is 534 px. Edit adds a row, 562 px. A
cylinder adds Round and Along, 618 px, the one case over 600 px, and it drops the Tones group
for an SVG. The tallest state is the Depth and print limits group open, six rows more, 702 px
(B1, B2).

## Text Decal, the walk

Steps 0 and 1 are the Image Decal steps with "Text Decal" for the name. Keep-outs, depth and
colour, seeing it, the readout, OK and coming back are the Image Decal steps 5 to 10 with the
words for the file (contradiction 13). The steps below are the text's own.

### Step T2. The words

**Sees** a row **Text**, a box two lines tall that grows to four. It opens holding the last
words used, all selected, so typing replaces them and a repeat label is one click (B36).

**Wants** to type and see the word on the face at once.

**Add-in does**
1. The words are laid out after a pause in typing of 0.3 s, not on every keystroke, because
   each layout costs about half a second on Fusion's thread (B14). The preview shows the words
   at the default height, centred.
2. Several lines are several lines. Shift+Return starts a new line, Return presses OK, as in
   Fusion's own dialogs, and the row's tooltip says so (B16). Build step 1 proves which key
   Fusion gives the box and the tooltip follows the truth.
3. An empty box shows nothing, line 4 says "Type the words" in plain text, and OK is disabled
   without a red line (B18).

**Goes wrong** when the font lacks a character. Fusion lays it out as a box, and line 4 names
the character in orange with the fix, "No 'é' in this font. Pick another font."

### Step T3. Font and style

**Sees** a row **Font**, a dropdown of the fonts Fusion lists (`Application.fontNames`), the
five last used at the top above a line, and "Find a font…" as its first item. Under it one row
of buttons, **B**, **I**, then **Left**, **Centre**, **Right**, then **◀** and **▶**.

**Wants** to try fonts quickly and see each on the face, and to find one by name among some
hundreds.

**Add-in does**
1. Each change lays the words out again in a scratch sketch, reads the loops back, re-traces
   and redraws, about half a second for a word. The font and style last used are the defaults
   next time. The first run uses Arial when it is installed, else the first font (B19).
2. ◀ and ▶ step to the previous and next font in the list, so trying ten fonts is ten clicks
   while watching the face (B15).
3. Picking "Find a font…" shows a text row under Font. Each keystroke narrows the dropdown to
   the fonts whose names contain the text, and the first match is applied live (B15). Clearing
   the text restores the full list and hides the row.
4. Left, Centre and Right are one **Align** control for both things a user means by it (A9).
   They set how several lines sit against each other, and they set the point the handles hold,
   the centre of the text box or its left or right edge at mid-height. Changing the words
   keeps that point still, so a left-aligned label grows to the right.
5. When B or I changes nothing in the laid-out loops, the font has no such face, and line 4
   says so in orange, "This font has no bold. Pick another or use a larger height." (B19).

**Goes wrong** when Fusion cannot lay the font out. Line 4 names the font, orange while the
previous font still stands on the face (B38).

### Step T4. Height

**Sees** a row **Height** in mm, the height of a capital letter, with its own arrow at the left
of the words pointing up. Line 2 shows the box the words make, "62 × 6 mm", so a lowercase
word's real height is on view beside the capital height typed (A16). **Letter spacing** in %
sits in the group Spacing, depth and print limits.

**Wants** "capitals 8 mm tall", and to know whether the line fits the face.

**Add-in does**
1. Height is the cap height. The add-in lays out "H" once per font and style, measures it,
   and scales Fusion's text height so the capitals come out at the number typed.
2. The default height is a fifth of the face's shorter side, reduced until the widest line fits
   the face width less two margins, and never under 4 mm.
3. The arrow drags Height. Width is read on line 2, not typed, because it changes with every
   keystroke and font.

**Goes wrong** when strokes fall under the 0.8 mm smallest line and vanish in the trace. Line 4
says in orange "Thinnest strokes 0.5 mm will not print. Bold, or height over 10 mm.", with the
height worked out from the thinnest stroke the trace found. With under half the letters left,
red with the same fix.

### Step T5. Where

**Sees** the triad and the Place group as in Image Decal, with X, Y and Angle, the button row
Centre, Place by click and, in Edit, Delete. There is no Width row and no Fit to face, since
the words' size is Height (contradiction 14). Mirror is a tick in Spacing, depth and print
limits (A12).

**Wants** the words where they go.

**Add-in does** as Image Decal step 3, with the Align point as the point the handles hold.

### Step T6. Along an arc

**Sees** a collapsed group with a tick in its header, **Along an arc (round a hole)** ☐, shown
on flat faces only, since a cylinder already wraps the words (A11). Inside, **Around**, a pick
of one circular edge anywhere in the design, then **Radius** in mm and **Words read** Over the
hole | Under the hole.

**Wants** words round a knob or a round boss, reading the right way up over it or under it,
centred on the hole without eyeballing.

**Add-in does**
1. The words are laid on a scratch arc of that radius with Fusion's along-path text, centred.
   Over the hole puts them along the top of the circle with their tops pointing away from the
   centre. Under the hole puts them along the bottom of the circle with their tops pointing
   toward the centre, as on the bottom of a coin. Both sit outside the hole by its keep-out
   margin, and both read left to right the right way up (B22).
2. Around takes any circular edge, so the knob's own skirt can be picked even when it covers
   the hole. The circle's centre is projected onto the face along the normal and its radius is
   the starting radius (B21).
3. With Around set, Radius defaults to the circle's radius plus the margin plus 1 mm, and
   Height is reduced if needed so the words sweep at most 120°, so the first thing on screen is
   the words in place and never a refusal (B20). The triad sits at the hole's centre and shows
   only the ring. Angle shifts the words round the hole from the top (Over) or the bottom
   (Under) by up to ±90°, and the words stay upright for the side they are on (B22). The Height
   arrow stays.
4. With nothing in Around, the arc's centre is 30 mm below the words' middle (Over) or above it
   (Under), and the triad holds the words' middle as for straight text.
5. Line 2 adds the sweep in degrees.

**Goes wrong** when the words are longer than the circle. Red, "Sweep 380°. Radius at least
12 mm, or height under 6 mm." (B39).

## Text Decal dialog

| Row | Open at start | Kind | Default |
|---|---|---|---|
| Face | yes | one flat or cylindrical face | preselection |
| Edit | only when the body carries decals | dropdown, New + every decal's name | New |
| Text | yes, two rows | box, 2 to 4 lines | last words, selected |
| Font | yes | dropdown, Find a font… first, recent on top | last used, first run Arial |
| Find font | only after Find a font… | text, filters the dropdown | |
| B · I · Left · Centre · Right · ◀ · ▶ | yes | button row | off, Centre |
| Height | yes | mm, cap height, with its arrow | fitted, at least 4 mm |
| Colour | yes | two swatches | last used |
| Place ▾ | open | group | |
| X, Y, Angle | | the triad's rows, named by Fusion | 0, 0, 0° |
| Round, Along | cylinders only | mm | 0, 0 |
| Centre · Place by click · Delete | | button row; Delete only in Edit | |
| Along an arc (round a hole) ☐ ▸ | collapsed, tick in header, flat faces only | group | off |
| Around | | one circular edge, optional | none |
| Radius | | mm | 30, or from the circle |
| Words read | | Over the hole / Under the hole | Over the hole |
| Keep clear of holes and edges ☑ ▸ | as Image Decal | | |
| Spacing, depth and print limits ▸ | collapsed | group | |
| Letter spacing | | % | 0 |
| Depth, Smallest line, Smallest speck, Mirror, Measure from | | as Image Decal | |
| Name, freeze and preview ▸ | as Image Decal | | Name defaults to the words |
| Readout | yes | four lines, full width | |

Rows at open on a flat face are Face, Text (two), Font, the button row, Height, Colour, the
Place header, X, Y, Angle, the button row, and the four group headers, fifteen, plus the
readout, sixteen rows of which the text box's second row is 28 px, so 565 px. Edit adds a row,
593 px. On a cylinder the arc group goes and Round and Along come, 593 px. The tallest state is
the Spacing, depth and print limits group open, seven rows more, 761 px (B1, B2).

## Several decals on one body

The first build suppressed every earlier decal on a body when a new one was cut, because each
decal replaces the parametric body with a stored copy and a later re-cut of an earlier decal
would have pulled the ground from under the later ones. From the user's side that rule meant a
lid could not carry the cane and a name at once, which is the first thing the lid wants (A7).
The rule is replaced.

1. Decals on a body form a stack in the order they were cut. A new decal cuts the body as it
   stands, stored copy and all, and goes on top.
2. Editing or deleting a decal re-cuts it and then every decal above it, in order, from their
   stored settings and files. Line 3 names the count and the summed time before OK, and the
   progress bar names each decal as it goes. One undo takes the whole run back.
3. When a new decal overlaps an earlier one, a tick **Clear earlier decals under it** appears
   in the Place group, on by default. On, the run re-cuts the earlier decals with this decal's
   outline grown by the margin as a keep-out, so a name over the cane sits in clear ground,
   and the preview hides the earlier pieces under that grown outline so what is drawn is what
   is cut. Off, the later cut runs through the earlier recess and the earlier colour stays
   where they overlap, the preview shows that, and line 4 warns in orange with the area,
   "overlaps cane by 12 mm²" (A8, B25, contradiction 17). Line 3 counts the re-cut either way.
4. A decal with a live cut (Freeze the cut off) sits in the stack like any other, and the stack
   still re-cuts above it.
5. Black and white decals stack freely, since each has its own inlay body and appearance.

## The preview

1. Three layers. The pieces, filled in the inlay colour at 90 % and edged in the opposite tone
   (B17). The frame of the whole art, a thin line in Fusion's selection blue. The keep-outs,
   red at 25 %. While a trace is pending the pieces turn grey.
2. Move and turn only redraw, in under 0.1 s. Width, height, tones, keep-outs, the print
   limits, words, font and arc re-trace. The decode and the re-trace run in a worker thread
   (`worker.py`) because both are plain Python with no Fusion call (B8). The result comes back
   on Fusion's thread through a custom event (`registerCustomEvent`, `fireCustomEvent`). Each
   change bumps a generation number, the command's end bumps it too, and the handler drops any
   result from an older generation or arriving when no command is live, so nothing is drawn
   after Cancel (B35). The thread checks the number in the core's progress callback and stops
   early.
3. The SVG import and the text layout stay on Fusion's thread, each about half a second, after
   the 0.3 s typing pause (B14).
4. OK is disabled while a trace is pending, and line 4 says "Updating…" with the time so far.
5. The thumbnail is written by the add-in's own PNG writer to `tools/decal/.cache/` under a
   new name each trace, the old file deleted, and set on `ImageCommandInput.imageFile`.
6. On a cylinder every vertex of the preview is mapped onto the surface by arc length.
7. If the custom-event path fails on the Mac in the first build step, the fallback is the
   same work on Fusion's thread with "Updating…" shown before it starts. The dialog stays
   correct and only the slider feels slower.

## Placement arithmetic, `decal_core/place.py`

All of it is pure functions with tests under ctest.

1. **Frame.** Origin, x, y, z of a flat face from its outer loop and the component axes, with
   the upright turn against the camera's up vector (`Camera.upVector`). For a cylinder, axis,
   radius, the seam angle under the first click, the unroll both ways, and the same upright
   turn for its axis (B33).
2. **Fit.** The largest width at a given angle and aspect whose box lies inside the face's
   raster mask less the margin, by bisection on the mask (B33). On a cylinder the mask is the
   unrolled face.
3. **Triad in.** From `lastChangeMade`, `transform` and `lastTransform` to a move by (dx, dy)
   or a turn by dθ. On a cylinder dx is arc length (B31).
4. **Handles out.** The triad's transform on a cylinder re-seat, and the origin, direction and
   value of the Width and Height arrows from the decal's box.
5. **Distances.** The decal box's distance to the two nearest face edges in the chosen Measure
   from frame, and the clearance to the nearest hole, for line 2 (A2, B12).
6. **Assembly context.** The lid is a component inside the enclosure, so the face comes with
   an occurrence transform. The frame, the fit and the trace work in the component's own
   space, and the handles and the preview are carried to root space through that transform.
   The sketch and the bake happen in the component, as the first build did on the root (A15).

## Edit and storage

1. The settings and the frame are stored as the earlier spec says, in attributes on the inlay
   body and in a design attribute index. Each record carries its kind, `image` or `text`, its
   name, its place in the body's stack, and a fingerprint of the face it was cut into (the
   outer loop's box and the list of holes with their sizes and centres).
2. The Edit row lists every record on the picked body (B27). Picking one of the dialog's own
   kind loads every value, re-finds the face, seats the handles and draws the preview. One of
   the other kind can be deleted here and is edited in the other add-in, which line 4 names.
3. OK on an edited decal deletes its group, bakes again under the same name and re-cuts the
   stack above it. Cancel leaves everything as it was.
4. A frozen cut does not follow a change to the face, and the body on screen is the top
   decal's stored copy, which never shows that change (B24). So the stale check and the Edit
   preview read the body as it stood just before the lowest decal on it. The add-in moves the
   timeline marker to just before that decal's group, reads the face (its box and holes, a
   few milliseconds), and moves the marker back to the end. Frozen cuts are base features, so
   the move back costs no rebuild of the heavy sketch. At every open the fingerprints are
   compared with that reading and line 4 names the stale decals and the change found (A6).
   Build step 1 measures the marker round trip on the enclosure and the budget is 1 s.
5. A group deleted by hand in the timeline leaves its record behind, and the decals above it
   still carry its cut in their stored bodies. At the next open line 4 says "cane's group was
   deleted. Edit any decal above it, OK re-cuts without it.", and the record is dropped once
   that run has happened.

## Errors and messages

The earlier spec's table stands for the causes, with these changes. Every message lives on
line 4 of the readout, at most 70 characters, in two parts, what happened and the change that
fixes it with a number where one exists (B39). The one message box is a failure inside the
bake. Rows that this spec handles differently from the earlier table are marked.

| Case | Colour | Message names |
|---|---|---|
| Preselected face of the wrong kind | red | its surface type and what to pick |
| More than one face preselected (earlier table refused) | orange | which was taken |
| A new file or font fails while one is in use (B38) | orange | the new one's fault, and that the earlier stays |
| A new file fails with nothing loaded | red | the file's fault and the fix |
| JPEG chosen (B29) | red | export as PNG from Preview |
| PNG over 25 million pixels | red | its size and the size to scale to |
| Image all one tone (earlier table refused) | orange | Light areas, or which way to slide Ink |
| Pieces off the face or in a keep-out (earlier table refused art wider than the face) | orange | the count and the move that brings the box in |
| Pieces under the smallest line | orange | the count and the width or height that keeps them; for words the thinnest stroke |
| Nothing left to cut | red | Centre, or the width that fixes it |
| Art wider than a cylinder's round | red | both numbers |
| Over a ceiling | red | the count, the limit, and the row and value that bring it under |
| Keep-out set under its hole | orange | the hole size it was set back to |
| Depth under 0.2 mm or over the wall | red | both numbers and the depth to use |
| A character the font lacks | orange | the character, pick another font |
| A font with no bold or italic face | orange | the font, pick another or a larger height |
| Words longer than the arc's circle | red | the sweep, the radius or height that fits |
| A decal overlaps an earlier one with Clear off | orange | the earlier decal's name and the area |
| A decal is stale after a face change | orange | the decal and the change found, Edit → OK |
| A decal's group was deleted by hand | orange | the decal and the re-cut that clears it |
| A file or font missing anywhere in a re-cut run (B40) | red | the decal and the path or font |
| Edit or Delete re-cuts a stack | plain, line 3 | the count of decals and the summed time |
| Cut preview dropped by a change | plain | that OK cuts again, and the time |
| Cut preview or bake fails | red, and a box for the bake | the feature and Fusion's message |

## Performance budget

| Action | Must finish in | Measured |
|---|---|---|
| Move, turn redraw | 0.1 s | to measure |
| PNG decode, 25 million pixels, in the thread (B8) | 5 s, with "Reading…" shown | to measure |
| Re-trace, one word | 0.5 s | to measure |
| Re-trace, full lid face, Dark | 3 s, in the thread | 0.4 s (2026-11-04) |
| Re-trace, full lid face, Photo relief | 3 s, in the thread | 2.3 s |
| Extra plan run for the ceiling hint | 3 s, in the thread | to measure |
| Thumbnail | 0.2 s | to measure |
| Text layout in the scratch sketch, after the 0.3 s pause | 0.5 s | 0.46 s sketch time for "cubevox" |
| Stale check, marker round trip (B24) | 1 s at open | to measure |
| Bake | the earlier spec's table | 2.8 s for a word, 52 s for the cane proof |

## Fusion API the design rests on

| Need | API | Standing |
|---|---|---|
| Drag handles for move and turn | `TriadCommandInput`, `addTriadCommandInput(id, transform)`, `hideAllScaling`, per-control visibility, `lastChangeMade`, `transform`, `lastTransform` | in the stubs of the installed Fusion. Never used here. The first build step proves it, and proves that its X, Y and Angle rows read from the creation frame (B11) |
| Width and Height arrows | `DistanceValueCommandInput.setManipulator(origin, direction)` | in the stubs |
| Collapsed group with a tick in its header | `GroupCommandInput.isExpanded`, `isEnabledCheckBoxDisplayed` | in the stubs |
| Thumbnail in the dialog | `ImageCommandInput.imageFile`, `scaleFactor` | in the stubs. To prove that a new path refreshes the picture |
| Dialog size | `Command.setDialogInitialSize`, `setDialogMinimumSize` | in the stubs |
| Full-width readout | `CommandInput.isFullWidth` | in the stubs |
| Preselected face, and Place by click | `SelectionCommandInput.isUseCurrentSelections` (marked preview by Autodesk, default true), `Selection.point` | in the stubs |
| Around pick | `SelectionCommandInput` with the `CircularEdges` filter | in the stubs |
| Multi-line text box | `TextBoxCommandInput` with `isReadOnly` false | in the stubs. To prove it fires `inputChanged` per keystroke and which key makes a new line (B16) |
| Fonts | `Application.fontNames` | used 2026-11-04 |
| Text layout | `SketchTexts.createInput2`, `SketchTextInput.fontName`, `textStyle`, `height2`, `setAsMultiLine`, `setAsAlongPath` | `createInput2` used 2026-11-04, the rest in the stubs |
| Thread result on Fusion's thread | `Application.registerCustomEvent`, `fireCustomEvent` | in the stubs, Autodesk's documented pattern |
| Cut preview reused by OK | `CommandEventArgs.isValidResult` | in the stubs |
| Drawn preview | `CustomGraphicsGroup.addMesh`, `addLines`, `setOpacity` | used 2026-11-04 |
| Upright to the camera | `Camera.upVector` | in the stubs |
| Stale check | `Timeline.markerPosition` | in the stubs |
| Buttons on the panel face | `CommandControl.isPromoted` | in the stubs |
| Status line after OK | `UserInterface.statusMessage` | in the stubs |
| Slider end labels | `SliderCommandInput.setText(left, right)` | in the stubs |
| Button row | `ButtonRowCommandInput` with icons | in the stubs |

Autodesk marks `CustomFeature` and its definition "preview, do not use in distributed
programs". It is not used (decision 3).

## Changes to the earlier pipeline

The core of the earlier spec keeps its shape. These are the changes this walk needs, each a
core or bake task when the build starts.

1. Rotation by any angle, not quarter turns. The source raster is turned at 0.1 mm with area
   sampling (contradiction 22).
2. The fit is found on the face's raster mask, not on its bounding box (B33).
3. The art's centre is the decal's centre, not the clicked point, and on a cylinder the
   tangent plane follows the centre as it moves (contradictions 22, 23).
4. The PNG decode runs in the worker with the trace (B8).
5. The trace reports the thinnest stroke among the pieces the smallest line removed, from the
   distance transform it already builds, and the width or height that would keep them (B39).
6. Keep-outs take extra loops. The outline of a later decal grown by the margin, for Clear
   earlier decals under it (B25).
7. The plan reports the distance from the art's box to the face edges and to the nearest hole
   (A2, B12), and the overlap area with earlier decals' pieces.
8. The bake runs a stack. Delete the edited decal's group and every group above it, bake each
   in order, as one command (A7).
9. Messages move from message boxes to the readout; only a failure inside the bake keeps a box
   (contradiction 19).
10. The preview draws in the inlay colour with edge lines, not orange (decision 8).

## Decisions and why

1. **Two add-ins** (owner). The shared code sits in two sibling packages so neither add-in
   carries the other's rows, and each shows one button.
2. **One triad for move and turn, an arrow for size.** Fusion's own Move command uses the
   triad, so the user already knows it. Size is a length in mm, so it gets a length handle and
   no scale factor is shown (B11).
3. **Edit through the dialog, not Fusion custom features.** Custom features would give
   double-click edit on the timeline and a recompute when the face changes, but Autodesk marks
   them preview and may change them. The Edit row gives the same dialog with one extra click.
4. **Cap height for text size.** Users say "8 mm letters" and mean the capitals. Fusion's text
   height is the em box and comes out smaller than expected.
5. **The thumbnail is the trace, not the source.** The question at that moment is what will be
   cut, and the Ink slider is then visible in the dialog as well as on the face.
6. **Tones named by result.** Dark areas, Light areas and Photo relief, with an Ink slider,
   replace Dark, Shading, Threshold and an Invert tick, because the user thinks in which part
   becomes inlay and how much (B6).
7. **The keep-out tick in the group header.** The common case is on and untouched. The second
   most common is off, one click, without opening anything.
8. **Preview in the inlay colour with an edge in the opposite tone**, not the orange of the
   first build. The user judges black on white or white on black, and orange answers neither
   (B17).
9. **Width in the readout for text.** It changes with every keystroke and font and is read, not
   typed, so it is not a row.
10. **Arcs only**, no free curves. The box's round features are circles. A free path needs a
    sketch pick and a second way to place, which the walk does not ask for.
11. **Names.** Image Decal and Text Decal, so neither collides with Fusion's own Insert → Decal
    command.
12. **Face centred by default, not click centred.** The click is a face pick, not a placement.
    Placement by click is its own button that asks for the click (B10).
13. **Freeze only when it pays.** A frozen cut hides later edits to the face. A small decal
    re-computes in under a second live, so freezing is on by default only when the bake is over
    10 s (B23).
14. **The stack re-cuts above an edit** rather than forbidding more than one decal per body
    (A7), and clears earlier decals under a new one by default (B25), because that is what a
    name over a pattern means.
15. **The Fusion app's installed Python stubs are the API reference** for what exists
    (`webdeploy/production/<hash>/Autodesk Fusion.app/Contents/Api/Python/packages/adsk`,
    build of 2026-10-21). Nothing in this spec names a call that is not in them.

## Not in this build

1. Fusion custom features (decision 3).
2. Dropping a file onto the dialog. Fusion's dialogs do not take drops.
3. Nudging with the arrow keys.
4. Text on a free sketch curve, and line spacing for several lines (Fusion's API has no line
   spacing).
5. More than two colours, or splitting a colour image into several inlays.
6. JPEG decoding. A plain-Python JPEG decoder is not small, and Preview exports PNG. A JPEG is
   named and refused (B29).
7. Outlined or hollow letters. The trace cuts what the font fills.
8. Snapping while dragging. Fusion's triad has no snap hook. Centre, Place by click, Around,
   Measure from and the typed numbers cover the exact cases.
9. A note in Fusion's status line the moment an upstream edit changes a face under a frozen
   decal. That needs a hook on every command Fusion runs. The freeze default (decision 13) and
   the stale check at open cover it (B23).

## Build order, when the owner says build

1. The shared dialog skeleton on the scratch plate. Face, triad, width arrow, drawn preview,
   worker thread, readout. It also proves the open points of the API table (the triad's rows,
   the text box's keys, the thumbnail refresh, the custom event) and measures the marker round
   trip. The owner drives it at the dialog before anything else is built, because the dialog
   is the thing the first build never proved.
2. Image Decal on that skeleton. Owner's check with the cane proof PNG on the lid.
3. Text Decal. Owner's check with "cubevox" on the lid and a word round the cylinder.
4. Edit, Delete, the stack and the stale check, in both.
5. Removal of the one-button `Decal` add-in and its symlink.

## Checks

1. `decal_core_test` under ctest, with the new `place.py` tests. The frame of the scratch plate
   and of a 40 mm cylinder, the fit at 0° and 30° on a rectangle and on a disc, each triad
   change round-tripping within 0.01 mm and 0.01°, arc length round the cylinder, the edge and
   hole distances, and the thinnest-stroke figure on a bar of known width.
2. The lead in Fusion, through the script runner. `api.run_decal` and `api.rebake` as before,
   a stack of two decals re-cut from the lower one, and the dialog's rows read through the
   active command after the owner opens it.
3. The owner's walk, which is the acceptance.
   1. With the lid's top face selected, Image Decal opens shorter than the screen.
   2. Choose the cane proof. Within two seconds the cane is on the lid, black, fitted, upright.
   3. Drag the dot. It follows the mouse. Drag the ring. It turns. Drag the width arrow. It
      shrinks and line 3's count changes when it is done.
   4. Type 40 in Width. The arrow and the decal jump to 40 mm. Line 2 gives the edge gaps.
   5. Move the Ink slider. The thumbnail and the lid change together.
   6. Untick Keep clear of holes and edges. The red tint and the gaps round the holes go.
   7. Press OK. The progress bar runs, the group appears, the inlay is black. Cmd+Z removes it.
   8. Text Decal with the same face. The last words are there, selected. Type "cubevox". It
      appears centred. Press ▶ three times. Tick Along an arc, pick a knob hole, choose Under
      the hole. The word sits under the hole, upright. Press OK.
   9. Image Decal again on the same face. Edit lists the cane and the word. Pick the cane. The
      handles sit on it. Move it. Line 3 says it re-cuts the word too. OK. Both are back in
      place and the word sits in clear ground.
   10. Click the word's inlay body with nothing else selected. The dialog keeps New and line 4
       names the word. Edit → the word, Delete, OK. The word is gone and the cane is whole.

## Open

1. Filament colours (owner).
2. The default arc radius of 30 mm when nothing is picked in Around, once the first label is
   placed.
3. Whether the points and pieces ceilings rise, as the earlier spec's Open 1 says.
