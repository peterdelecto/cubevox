# Circle inlay on the lid face (C12)

Owner 2026-11-02. The lid face carries the C12 pattern: a crumpled-tissue relief drawn in
nothing but separate light circles on the dark lid, 0.94 to 2.66 mm across, lit from the upper
right. It replaces the cane weave of `2026-11-02-cane-face-design.md` on the face. The cane
build stays in the Fusion timeline, suppressed.

## Decisions (owner)

1. Flush inlay 0.4 mm deep (`inlayDepth`), face printed down on the bed, as for the cane.
2. The pattern is candidate C12 as proofed in `build/cane/candidates/C12.png`, chosen
   2026-11-02 over C5 to C10 and G5.
3. Everything light is a circle. No light ground, no ovals, no merged shapes.
4. Two colours: light circles, dark lid.
5. The 3 mm border, the 1 mm clearance round every hole and the OLED window, and no labels,
   as for the cane.

## Geometry

| Item | Value |
|---|---|
| Circles | 2,307 |
| Diameter | 0.94 to 2.66 mm, in steps the 0.8 mm print opening leaves whole |
| Dark gap between big circles | 0.65 mm or more; the small gap fillers keep 0.7 mm to every neighbour |
| Margin past the circle's edge to the border, the clearance rings and the OLED clearance | 0.15 mm |
| Depth | 0.4 mm (`inlayDepth`) |
| Face in the Lid component | x -3.5 to 188.5, y -148.5 to 3.5; the pattern field is 3 mm in |

1. `tools/cane-weave/candidates_c.py` holds the generator, `c12_circle_list()`. C4's tissue
   height field is lit from 315 degrees. A Poisson site set 2.4 to 2.7 mm apart gets one circle
   per site, grown with the light from 0.9 mm to the room its neighbours leave less 0.65 mm,
   and thinned to bare dark below tone 0.2. Above tone 0.45, circles of 0.94 to 1.3 mm fill the
   gaps. Every radius is snapped down to a size the print opening leaves whole. The seeds are
   fixed, so the list is reproducible.
2. `tools/cane-weave/c12_export.py` writes the list as `build/cane/c12-circles.json`: centre
   x, y and radius in mm in the Lid component's coordinates. The image's u (right) and v (down)
   from the face's top-left corner map to x = u - 3.5 and y = 3.5 - v.
3. The circles lie wholly inside the allowed field, so the Fusion build needs no trim.

## Fusion build (Lid component)

1. Sketch `C12` on the lid's top face (z = 13.2662 mm): one circle per list entry, drawn in
   sketch space with z = 0, compute deferred while drawing.
2. Extrude `C12 cut`: the 2,307 circle profiles cut 0.4 mm into the lid body.
3. Base feature `C12 dots`: one body, named `C12 dots`, of 2,307 lumps, the union of
   temporary 0.4 mm cylinders made from the list. OrcaSlicer chokes on thousands of bodies
   (owner 2026-11-02). A parametric extrude gives one body per dot, because Fusion keeps every
   separate lump as its own body and Combine will not join bodies that do not touch. The base
   feature body is not parametric; the script remakes it when the list changes.
4. The three features sit in one timeline group, `C12`, after the cane group `Group3`, which
   stays suppressed.
5. The build script is `tools/cane-weave/fusion_c12.py`, run through the Fusion MCP. It reads
   the JSON by absolute path. The cut takes over a minute and outlives the MCP call's timeout;
   the script skips stages that already exist, so a rerun carries on.

Built 2026-11-02 (document not saved): the Lid holds two bodies, the lid and `C12 dots`. The
dots body has 2,307 lumps, spans z 12.8662 to 13.2662 mm and x -0.36 to 185.38,
y -145.38 to 0.38; its volume is 2,092.5 mm³, the sum of pi r squared times 0.4 mm. The lid
fell from 75,815.3 to 73,722.9 mm³, the same amount, and the lid and the dots share no volume.
Top view: `build/cane/c12-lid.png`.

## Print

The lid body prints in the dark filament and the dots body in the light filament, two parts
of one object in the slicer. Each body exports as one STL from Fusion. The smallest dot is 0.94 mm across, two 0.2 mm layers deep.

## Open

1. Filament colours (owner).
2. Saving the Fusion document (owner).
