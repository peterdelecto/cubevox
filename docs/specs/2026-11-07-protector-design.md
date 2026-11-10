# Protector

A printed 68D TPU cup with two jobs. In use the box stands in it (the nester): the cup holds the
boot and the plate is the box's foot. For travel the same cup turns over and goes down over the
knobs (the protector). In both positions it locks into the TPU boot and touches nothing else:
two notches on each side wall drop into the two windows the owner cut in the boot's side walls.
The lid and dish carry no feature for it (owner 2026-11-07; the lid bumps and the dish cut of
2026-11-06 were never part of the design). Owner decisions 2026-11-07: starting over; the boot
has the windows; the protector has the notches that lock into the boot.

## Adam's walk

1. In use. The box stands in the protector on the table. He sees the boot sunk in a TPU tray
   whose rim stops 1 mm under the lid face (owner 2026-11-07: a rim above the face is
   unacceptable). Nothing of the tray stands over the face. He plays the knobs and toggles as
   before.
2. Gig over. He unplugs the leads, lifts the box out of the tray. The box comes up 1 mm, the
   notches catch in the boot windows and the tray lifts with it. He pulls harder and the
   notches ride out over the 1.25 mm chamfer of the windows; the tray drops onto the table.
3. He turns the tray over, holds it above the box with the open side at the back where the
   jacks are, and lowers it. The walls pass the lid and dish with 2.4 mm to spare, meet the
   boot's tongues at the seam, ride over them with the wall stretching 1.4 mm, and the lower
   notches drop into the windows. The plate stops 4 mm over the knobs.
   Goes wrong: turned back to front. The closed front wall meets the jacks at the back and
   stops with its rim on the jack bodies 25 mm up. Nothing bends; he turns it round.
   Goes wrong: left to right. The notches sit on the box's long centre line, so there is no
   wrong way round left to right.
4. In the bag. The knobs are under 3 mm of 68D TPU with 4 mm of air. The protector hangs from
   the two notches resting on the bottom edge of the windows, and its rim ends 4.6 mm under
   the lid seam, 2.5 mm below the notches. The cup can rock 4 degrees front to back on the notches
   before the wall touches the boot's front face; the owner can add front windows in the boot if that
   rattles.
   Goes wrong: something heavy lands on the bag. The plate flexes; 4 mm of air and a 3 mm
   plate keep a hand's push off the knobs.
5. Next gig. He puts the box down on its boot, grips the protector's walls low and pulls up.
   The notches ride out of the windows and the cup comes off straight up. He turns it over,
   sets it on the table and stands the box in it; the upper notches drop into the windows
   and the box is in its tray again.

## Numbers from the model and the owner

| Item | Value | Source |
|---|---|---|
| Lid face | z 13.8 mm, level | Fusion `cubevox-enclosure`, Lid |
| Lid and dish side walls | x -3.5 / 188.5; front y -148.5; rear y 3.5 | Fusion |
| Boot outer side walls | x -5.5 / 190.5 (2.0 mm proud of the lid); front y -150.5; rear face y 5.2 | Fusion, Boot |
| Boot band | 22 mm tall, tilted 2 deg with the floor; floor bottom z -34.3 rear to -29.4 front; top edge z -12.3 rear to -7.4 front | Fusion, Boot side faces |
| Boot floor bottom at the window centre (y -71.75) | z -31.9 | Fusion, floor plane |
| Boot pockets as cut 2026-11-07 (both side walls) | y -79.25 to -64.25, z -20.8 to -13.1, 1.25 mm deep | Fusion, Boot Extrude11, Extrude13, Chamfer6; out of reach from the top, see Decisions 2 |
| Lid seam | z 0.3; lid skirt over the dish rim z 0.3 to 3.0 | Fusion |
| Knob top over the lid face | 20 mm, z 33.8 | owner, ruler, 2026-11-05 |
| Toggle lever tip | z 25.2 | Fusion |
| Rear protrusions | jacks to y 8.4, z -29 to 3.5 | Fusion |
| Box corner radius | 6.5 mm | Fusion `boxCorner` |
| Printer bed | 254 x 254 mm | owner |

## Geometry

1. Plate. 3.0 mm, parallel to the boot floor (2 deg), so turned over it is lowest at the front.
   Nested, its inner face lies on the boot floor. On top, its inner face is 4 mm over the knob
   tops at the front knob row (y -135, z 37.8) and z 40.1 on the box's long centre line.
   Corners follow the walls.
2. Walls. 2.0 mm, square to the lid face, one straight run from the plate to the rim, the same
   size the whole way because the cup goes over the boot from both ends. Inner face 0.4 mm
   outside the boot: x -5.9 / 190.9, front y -150.9. Corner radius inside 8.9 mm (boot corner
   8.5 plus 0.4), outside 10.9. The gap to the lid and dish walls is 2.4 mm; the boot locates
   the cup.
3. Depth. Plate inner face to rim 44.7 mm on the long centre line; the rim is level with the
   lid face. Nested the rim stands at z 12.8, 1.0 mm under the face. On top the rim reaches
   z -4.6, under the lid seam and 5 mm above the boot's top edge. The rim rule sets the depth;
   the on-top lock then has to sit at the seam, so the boot comes up to it (item 6).
4. Notches. Four, two per side wall, on the box's long centre line y -71.75. Each 12 mm long,
   3.5 mm tall, 1.4 mm proud of the inner face (0.4 mm air plus 1.0 mm into the 1.25 mm
   pocket, 0.25 mm off the pocket floor), ends square, top and bottom faces chamfered 45 deg
   by 1.0 mm so they ride over the boot's edges. Left and right are mirrors.
5. Notch rows. Both rows land in the same windows, centred z -0.35. Row N (nested) centred
   31.6 mm above the plate inner face: with the box on the plate the notch sits on the window's
   bottom edge with 1.5 mm free above it; lifting the box pulls it onto the top edge. Row T
   (travel) centred 40.5 mm below the plate inner face: on top the notch rests on the window's
   bottom edge with the plate 4 mm over the front knobs, and 2.5 mm of wall below it to the
   rim. The unused row stands beside the lid skirt at z 8.6, 1.0 mm off it, touching nothing.
6. Boot windows. Two, one per side wall, on the long centre line, 14 mm long and 5 mm tall,
   bottom edge z -2.1, through the 2.0 mm wall, with the dish rim and lid skirt behind them.
   They sit above the boot's top edge, so the boot carries them on two tongues (A1) or on a
   taller band (A2); the owner's choice, Decisions 2. A1: a tongue 18 mm wide and 2.0 mm thick
   rising from the boot's top edge to z 4.9, lying on the flush dish and lid walls, 1.0 mm 45
   deg lead at the top. The pockets cut 2026-11-07 are not used.
7. Rear. Open. The side walls wrap the rear corners and end on the boot's rear face line,
   x 3 and 182, clear of the first jack by 7 mm. A lip 1.5 mm tall and 2.0 mm thick runs
   between the wall ends along the plate's rear edge, inner face 1.0 mm behind the boot's rear
   face. Nested it stands 1.5 mm up the boot's rear; on top it hangs 1.5 mm behind the lid.
8. Edges. 1.0 mm round on the plate's outer edges and the rim.
9. Outer size. 201 x 161 mm, 48 mm tall. Fits the 254 mm bed flat.

## Parameters in Fusion

| Name | Default | Sets |
|---|---|---|
| `protPlate` | 3.0 mm | plate thickness |
| `protWall` | 2.0 mm | wall thickness |
| `protClear` | 0.4 mm | air between the wall and the boot |
| `protDepth` | 44.7 mm | plate inner face to rim on the long centre line; rim 1.0 under the lid face nested |
| `knobClear` | 4.0 mm | air over the 20 mm knob on top, at the front knob row |
| `notchLen` | 12 mm | notch length along the wall |
| `notchHeight` | 3.5 mm | notch height |
| `notchProud` | 1.4 mm | notch stand-off from the inner face |
| `notchRowN` | 31.6 mm | row N centre above the plate inner face |
| `notchRowT` | 40.5 mm | row T centre below the plate inner face |
| `windowLen` | 14 mm | boot window length |
| `windowHeight` | 5 mm | boot window height |
| `windowBottom` | z -2.1 | boot window bottom edge, world |
| `tongueWidth` | 18 mm | A1 boot tongue width |
| `tongueTop` | z 4.9 | A1 boot tongue top edge, world |
| `protLip` | 1.5 mm | rear lip height |
| `protLipClear` | 1.0 mm | lip to the boot's rear face |
| `protEdge` | 1.0 mm | plate and rim rounds |
| `boxCorner` | 6.5 mm | corner radius of the lid, dish and boot walls |

## Print

1. TPU, Shore 68D, not glass-filled (owner 2026-11-05). 0.2 mm layers.
2. Plate down, walls up. The notches are 1.4 mm inward overhangs on 45 deg faces; no supports.
3. Plate and walls are explicit solids. No infill volume.
4. Fit is built for the first print: 0.4 mm air at the boot covers TPU swell; the stretch is at
   the notches, where both parts give.

## Fusion build

1. Component `Protector` in `cubevox-enclosure` beside Lid, Dish and Boot. No construction
   planes; every sketch sits on a face of the box.
2. Layout sketch on the lid face: the boot's outer outline projected, offset 0.4 for the inner
   wall and 2.4 for the outer, the rear ends on the boot's rear face line. Extrude the wall ring
   down to the plate and up to the rim.
3. Plate sketch on the boot's floor bottom face: the wall ring's projected outline. Extrude
   `protPlate` away from the boot. The lip from the same sketch.
4. Notch sketch on the left wall's inner face: two rectangles on the window centre line at
   `notchRowN` and `notchRowT`. Extrude `notchProud` as join. Chamfer the top and bottom edges.
   Mirror the two notches to the right wall across the box's centre plane.
5. Round the plate edges and the rim with `protEdge`.
6. A second occurrence of `Protector` turned a half turn about the box's long centre line and
   held to the Boot by a rigid as-built joint named `on top`, so both positions show. The
   nested occurrence stays where it was built.
7. STL to `hardware/exports/protector.stl`.

## Decisions

1. Lock into the boot only (owner 2026-11-07). The lid and dish are finished prints and carry
   nothing for the protector. The 2026-11-06 lid bumps and dish cut are gone with the revert.
2. Windows at the seam, open: the owner picks how the boot reaches them. A1, two tongues 18 mm
   wide rising 15 mm from the boot's top edge on the side walls; the rest of the boot stays
   as it is. A2, the whole band raised to z 4.9, covering the dish and 4.6 mm of the lid, with
   the rear cut round every jack and the USB-C. The pockets cut 2026-11-07 at z -13 to -21
   are out of reach: the deepest cup the rim rule allows ends at z -4.6 on top.
3. One wall size, boot size. The cup goes over the boot from both ends, so no step to the lid
   is possible; the 2.4 mm gap at the lid is the price.
4. Depth 44.7 mm (owner 2026-11-07: no rim above the lid face). From the boot floor to 1 mm
   under the face. On top this puts the rim 4.6 mm under the seam, which is why the windows
   sit at the seam.
5. Two notch rows. A single row cannot serve both ends of the cup because the plate is at one
   end; the unused row stands in free air above the face.
6. A3, open: windows in the boot's front wall would stop the 4 degree rock on top. Not cut.

## Status

Spec written 2026-11-07 from the owner's words and the Fusion model; rim rule folded in the
same day. Blocked on the owner's pick of A1 or A2 for the boot. Not built. Supersedes `2026-11-05-cover-design.md` whole.
