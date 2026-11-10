# Cover

A printed TPU cap, Shore 68D, not glass-filled, that sits over the knobs when the box travels in
Adam's bag. It grips the TPU boot on three walls and stops short of the jacks on the rear wall.
One print, no bought parts. Owner decisions 2026-11-05: cover not case; grip on the boot, which
stays on the box for good; lip on three walls, rear skirt short; 68D TPU, not PETG.

## Adam's walk

1. Gig over. Adam unplugs the mic, the 1/4" lead and the power lead. The box sits on the table
   with the knobs where he left them.
2. He picks the cover up by its sides, holds it over the box with the short wall at the back,
   and lowers it. The skirt finds the flush walls of the lid, slides down 21 mm, then the band
   at its foot meets the boot. He presses down; the band rides over the boot's top edge and the
   inner step lands on it. Nothing touches a knob or a toggle.
   Goes wrong: cover turned back to front. The short rear skirt is on the front and the long
   skirt hits the jacks. He turns it round. No damage, because the long skirt lands on the jack
   bodies, not the lever of the power switch, and the jacks sit in the box's own wall.
3. He drops the box in the bag. Knobs, toggles and the MENU encoder are under 3 mm of 68D TPU
   with 4 mm of air above them. The cover cannot slide off, because the band holds the boot
   on three walls, TPU on TPU, and the box's own weight sits on the rear wall side when the bag
   stands up.
   Goes wrong: something heavy lands on the bag. The plate flexes down. 4 mm of air and a 3 mm
   plate keep it off the knobs under a hand's push; a hard blow reaches a knob through a soft
   plate, which turns it rather than snaps it.
   Goes wrong: the cover rides up a few mm in the bag. The band is 5 mm tall, so the cover is
   still gripping with 3 mm of ride.
4. Next gig. Adam puts the box down, hooks his thumbs under the short rear skirt, which ends at
   the lid seam 13.5 mm below the face, and lifts. The band lets go of the boot and the cover
   comes off straight up. He sets it aside, face down on its plate.

## Numbers from the model and the owner

| Item | Value | Source |
|---|---|---|
| Lid and dish outer walls | 192 × 152 mm, flush, PETG | Fusion `cubevox-enclosure`, Lid and Dish bodies |
| Lid face | z 13.8 mm | Fusion |
| Lid seam | z 0.3 mm | Fusion |
| Boot top edge | z −7.1 mm, 2.0 mm proud of the walls on front and sides | Fusion Boot body |
| Knob top over the lid face | 20 mm | owner, ruler, 2026-11-05 |
| Toggle lever tip over the lid face | 11.4 mm | Fusion |
| Highest rear-wall protrusion | z −1.4 mm (power switch housing); 1/4" jack nose to −1.7 mm | Fusion |
| Printer bed | 254 × 254 mm | owner |

## Geometry

1. Plate. 3.0 mm thick, outside flat, inside flat. Inner ceiling 24 mm above the lid face
   (20 mm knob plus 4 mm air). The plate is thicker than the skirt because 68D TPU flexes over a
   190 mm span and the plate is what a knock in the bag presses on the knobs. Outer corners follow
   the box's corner radius.
2. Skirt, front and sides. 2.0 mm wall. Inner face 0.4 mm outside the lid and dish walls from the
   face down to the boot top edge (20.9 mm). At the boot top edge the inner face steps out 1.2 mm
   (the boot's 2.0 mm less `wallClear` and `lipBite`) through a 45° chamfer and runs 5 mm further
   down as the band.
3. Band. Inner face 0.4 mm inside the boot's outer face. Both parts give, the boot (TPU-GF, grippy,
   slightly compressible, owner 2026-11-05) and the cover (68D TPU), so the band stretches over the
   boot and TPU on TPU holds it. 1.0 mm 45° lead-in at the bottom edge. Runs the full front wall
   and both side walls, stopping 2 mm short of the rear corners.
4. Step. The 2.0 mm inner step above the band lands on the boot's top edge. It is the depth stop,
   so the plate never meets a knob.
5. Rear skirt. 2.0 mm wall, inner face 0.4 mm outside the lid wall, 13.5 mm deep, ending at the
   lid seam. No band. Its bottom edge is 1.7 mm above the power switch housing and clear of every
   jack. The side skirts run full depth to the rear corners; the rear skirt joins them there.
6. Outer size. About 199 × 159 mm, 53 mm tall. Fits the 254 mm bed flat.
7. Edges. 1.0 mm fillet on the outer edges of the plate and the skirt feet. No text, no inlay.

## Parameters in Fusion

| Name | Default | Sets |
|---|---|---|
| `coverPlate` | 3.0 mm | plate thickness |
| `coverWall` | 2.0 mm | skirt thickness |
| `knobClear` | 4.0 mm | air above the 20 mm knob |
| `wallClear` | 0.4 mm | gap to the flush PETG walls per side |
| `lipBite` | 0.4 mm | band interference over the boot |
| `bandHeight` | 5.0 mm | band run below the boot's top edge |
| `rearSkirt` | 13.5 mm | rear skirt depth from the lid face |

## Print

1. TPU, Shore 68D, not glass-filled (owner 2026-11-05). 0.2 mm layers.
2. Plate down on the bed, skirt up. The step and the band are inward overhangs of 2.0 and
   0.4 mm on 45° faces, so no supports, which TPU would not peel cleanly anyway.
3. Plate 3.0 mm and walls 2.0 mm are explicit solids. No infill volume.
4. Fit is built for the first print. `wallClear` 0.4 mm covers TPU swell on the skirt against the
   fixed PETG walls; the interference sits at the band, where both parts stretch.

## Fusion build

1. Component `Cover` in the `cubevox-enclosure` document, next to Lid, Dish and Boot.
2. One layout sketch on the lid face plane: the box outline, the skirt inner and outer outlines,
   the rear wall line.
3. One detail sketch on the part's centre plane (front to back): the plate, the skirt profile
   with its step, band and lead-in, the rear skirt depth. Revolve nothing; extrude the profile
   along the walls and close the corners.
4. Parameters above as user parameters.
5. STL to `hardware/exports/cover.stl`.

## Decisions

1. Cover, not case (owner 2026-11-05). The knobs are the fragile part; the box rides in Adam's
   bag as it is.
2. Grip on the boot (owner 2026-11-05). The boot stays on the box for good and the TPU is the one
   compliant surface on the outside. The PETG beads are inside the walls under the boot and cannot
   be used.
3. Band on three walls, rear skirt short (owner 2026-11-05). The rear jacks start 1.4 mm below the
   lid face, 6 mm above the boot's top edge, so a rear band would need five notches and thin strips
   between them.
4. Rear skirt ends at the lid seam. It gives thumbs a straight edge to lift from and keeps the
   skirt's foot one clean line.
5. 68D TPU, not PETG (owner 2026-11-05). The cover stretches over the boot instead of biting it,
   and TPU on TPU holds. The plate goes to 3 mm and the air over the knobs to 4 mm because the
   softer plate flexes under a knock.

## Status

Spec written 2026-11-05 from the owner's answers and the Fusion model. Not built. Superseded whole by `2026-11-07-protector-design.md` (owner 2026-11-07).
