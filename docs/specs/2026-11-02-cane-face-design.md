# Cane weave inlay on the lid face

Owner 2026-11-02. The lid face carries a flat cane webbing pattern printed in a second
colour. The strands are a cane colour, inlaid flush into the dark lid. The lid shows through
as the octagonal holes and as thin lines where one strand passes under another.

## Decisions (owner)

1. Flush inlay, about 0.4 mm deep (two 0.2 mm layers, face printed down on the bed).
2. Cane webbing as in the owner's photo: paired horizontal and vertical strands, single
   diagonals both ways, octagonal holes.
3. Two colours: cane strands, dark lid.
4. The weave covers the whole face, cut back around every pot, toggle, MENU and the OLED window.
5. No labels on the face for now.
6. A plain dark border about 3 mm wide frames the weave.
7. The weave sits square to the face edges.

## Geometry

| Item | Value |
|---|---|
| Repeat of the band lines | 12 mm (`canePitch`); half the 24 mm slot pitch |
| Pair strand width | 2.5 mm, two per band, 0.6 mm apart |
| Diagonal width | 2.2 mm |
| Gap each side of an over strand | 0.6 mm |
| Smallest piece kept | 0.8 mm wide; narrower slivers are left dark |
| Inlay depth | 0.4 mm (`inlayDepth`) |
| Border | 3 mm (`caneBorder`), offset in from the face outline |
| Clearance round each hole and the OLED window | 1 mm (`caneClear`) |

1. Band lines run at x = 2.5 + 12k and y = 5 + 12k on the face, so octagon centres land on
   every toggle and slot column (x = 8.5 + 24k, y = -85).
2. Diagonals run through the band midpoints and cut the octagon corners. The octagon comes out
   close to regular.
3. Over-and-under follows three rules. Diagonals lie over the pairs. Where two diagonals cross on
   a vertical band, "\" lies on top, and on a horizontal band "/" lies on top. At each junction one
   pair lies over the other, and the top pair alternates junction by junction. The repeat is
   therefore 24 mm square.
4. `tools/cane-weave/weave.py` computes the pieces as convex polygons. `weave.py proof out.png`
   draws the whole face for checking; `weave.py tile X0 Y0` prints one 24 mm repeat.

## Fusion build (Lid component)

1. Sketch `cane` on the lid's top face. It holds the projected face edges, the border offset,
   one clearance offset per hole and the OLED window, a frame offset `caneFrame` (45 mm) out
   from the face outline round the whole pattern area, and one 24 mm repeat drawn off the face
   at (-33.5, -163). The face sits at z = 13.2662 mm; the repeat's points lie in the sketch plane.
2. Extrude `cane tile`: the repeat's profiles, 0.4 mm into the lid, new bodies.
3. Rectangular pattern `cane pattern`: 10 x 7 repeats at 2 x `canePitch`.
4. Extrude `cane trim`: cuts the cane bodies only, outside the border, inside every clearance,
   and outside the face.
5. Combine `cane inlay`: cuts the cane bodies from the lid body, keeping the cane bodies.

Fusion keeps every separate strand piece as its own body, so the inlay is 1,201 bodies
(built 2026-11-02). The trim also leaves 497 empty bodies, with no faces, from the pieces it
removed whole. The combine removes 5,441.5 mm³ from the lid, the same as the cane volume.

The strand widths live in `weave.py`; changing them means re-running the build script for the
tile. Pitch, depth, border and clearance are user parameters.

## Print

The lid body prints in the dark filament and the cane bodies in the cane filament. The slicer
gets the lid and the cane as two parts of one object.

## Open

1. Filament colours (owner).
2. Knob skirts and toggle nuts cover the cut edges round the holes; the clearance is 1 mm until
   the knobs are chosen.
