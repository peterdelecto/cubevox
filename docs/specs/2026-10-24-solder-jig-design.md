# Solder jig for the underside jacks

Owner 2026-10-24. JLC assembles the top side. The owner then hand-solders four through-hole parts
that hang from the underside and face the rear wall: combo J101, 1/4" J108, barrel J106 and the
slide switch SWPWR1. This is the plan for the printed jig that holds them while they are soldered.
The jig is its own part. It is not the enclosure dish or lid.

## 1. What the jig must do

| # | Need | Why it matters | Target |
|---|---|---|---|
| 1 | Seat every part flat on the underside with no gap | A gap under a jack moves its mouth out of the rear-plate slot and loads the solder joints when a plug goes in | 0 mm gap, held by the board's weight, no clamps |
| 2 | Hold every part square to the board and to the rear edge | The lid's rear plate has one slot per jack; a tilted jack binds in its slot or shows a crooked mouth | mouth within 0.2 mm and 0.5 deg of nominal |
| 3 | Not fight the pins | Pins in plated holes already fix x and y; a pocket that also fixes x and y binds on print error | pocket clearance larger than hole slop; pins locate, pocket only stops tilt |
| 4 | Hold the board level and still | The owner needs both hands for iron and solder | board rests on fixed points, cannot rock or slide |
| 5 | Keep clear of what is already on the underside | JLC's pot and toggle pins stand about 2.4 mm proud with fillets; test pads and vias are there too | nothing touches the underside except at the four rest seats |
| 6 | Leave every pin reachable from the top | Soldering happens on the top side between the rear pot row and the edge | nothing of the jig rises above the board; the rear edge is open |
| 7 | Take the heat | Iron tip 350 C, pins conduct into the part body, PETG softens at about 80 C | no jig wall within 1 mm of a pin exit; floors thick enough that a hot body does not sag them |
| 8 | Let the parts load and unload by hand | The owner does several boards | parts drop in, board drops on, lifts off, no tools |
| 9 | Be printable at home in one go | One piece, no supports, PETG | flat bench face down, fits a 220 x 220 bed, no bridges wider than 10 mm |
| 10 | Be checkable before it is trusted | Pocket sizes come from vendor 3D models, not measured parts | a pocket coupon prints first and the real parts are tried in it |

## 2. Fixed facts from the board

1. Board 185 x 145 mm, 1.6 mm thick, rear edge at the top of the KiCad view. Four plain rest
   seats with 3.2 mm holes at (5, 5), (180, 5), (20.5, 140), (164.5, 140) mm from the rear-left
   corner. They carry no screws, so the jig may use them.
2. All four parts are on the underside with mouths past the rear edge. From the STEP in the
   Fusion document, lowest point under the board per part: combo body 25.1 mm, combo latch 29 mm
   (past the edge), 1/4" 12.7 mm, barrel 11.1 mm, switch 6.7 mm.
3. Everything else on the underside is pin stubs, about 2.4 mm, over the pot and toggle rows.
4. The 1/4" jack ships with a nut and washer on its bushing. The combo has three locating pegs
   that drop into unplated holes.

## 3. Design that follows

1. **Orientation.** Board face up, parts hanging below into pockets, soldering from the top. The
   other way round (board face down on its pots) cannot hold the board level and hides the pins.
2. **Datum.** The pocket floor. Each pocket is cut so its floor is exactly at the part's bottom
   face when the part is seated. The part rests on the floor and the board rests on the part. Gravity
   does the seating (need 1). Pocket walls get 0.25 mm per side and only stop tilt (needs 2, 3).
3. **Board rest.** Four bosses on the rest seats. The front pair carries the board at full height.
   The rear pair sits 0.3 mm low so it can never hold the board off the jacks. Two loose pegs,
   2.4 mm in the 3.2 mm holes, only stop the board sliding off the bosses. The parts in their
   pockets locate the board; the pegs do not (need 4). Nothing else touches the underside (need 5).
4. **Frame.** A ring, not a slab. Rear block 32 mm deep carries the pockets; 14 mm side and front
   rails carry the front bosses. Frame top 5 mm under the board to clear the pin stubs. Bench face
   32 mm under the board so the combo latch clears the bench by 3 mm.
5. **Pockets.** One per part, open through the rear face so mouths, latch, lever, nut and washer
   hang in free air (need 8). The combo pocket steps three times to follow the body, its side ears
   and its flange. Floors are 7 mm or more thick (need 7).
6. **Gauge lips.** At the rear face each pocket ends in a U-shaped lip rising from the floor,
   the lower half of that jack's lid cutout, 0.2 mm clearance at the bottom and 0.5 mm at the sides. The mouth drops into the U as the
   part loads. The U bottom checks height, the floor checks pitch, the loose sides catch gross yaw. Fine
   x and y come from the pins and are not the jig's job. A part that is not square does not go in. A soldered board dropped back on the jig is a
   go gauge (need 2). The U profiles come from the mouth geometry in the model now and from the
   lid's slot profile once the rear plate is drawn.
7. **Access.** The rear face is flush with the board edge and nothing rises above the frame top
   except the bosses and pegs (need 6).
8. **Parameters.** `jigPinGap` (5 mm) and `jigLift` (0.3 mm) drive the heights so a print that
   lands short or long is fixed by one number.

## 4. Build steps in Fusion

1. In `cubevox-enclosure`, new component `solder-jig` at the board's origin.
2. Plane at z -32, sketch the frame ring, extrude up to the frame top.
3. Sketch the four bosses on the frame top; extrude front pair to z 0, rear pair to z -0.3.
4. Plane at z -0.3, sketch the two 2.4 mm pegs, extrude 3.3 mm, chamfer the tips 0.5 mm.
5. Sketch the pockets on the frame top from the part bodies' bounding boxes, cut each to its
   floor depth with the body's bottom face as the reference.
5a. Sketch the gauge lips on the rear face from each mouth's section, extrude them 3 mm into the
   pocket so each lip stands on its floor.
6. Name every sketch and feature. Sizes the owner may tune go in user parameters.

## 5. Verification

1. **Model.** Interference analysis between the jig and the board plus the four part bodies must
   report zero overlaps. Minimum distance to each part body must read the wall clearance. Timeline
   healthy.
2. **Coupon.** Print the rear block alone (about 185 x 32 x 27 mm) and try the four real parts in
   it. Each must drop in and sit flat with no rock. Try the board on the pegs. Adjust `jigPinGap`,
   peg diameter or a pocket if anything binds, then print the full jig.
3. **First board.** Every mouth must sit down in its gauge lip before the iron touches anything.
   After soldering, lift the board off and drop it back on; every mouth must find its U again.

## 6. Use

1. Jig on the bench, pockets toward you. Nut and washer off the 1/4" jack.
2. Drop the four parts into their pockets, mouths out the rear face.
3. Lower the board face up onto the two pegs. Press over each jack until the board sits on the
   parts and the front bosses.
4. Solder every pin from the top. Lift the board off.

## 7. Printing

1. **Clearance.** FDM inner walls print about 0.1 to 0.2 mm small. Pocket walls get 0.4 mm per
   side, not 0.25. Tilt is held by the flat floor and the board, not the walls, so the extra
   clearance costs nothing.
2. **XY length does not matter.** Pins set each part's x and y to about 0.1 mm against the board.
   The jig has no tight locator anywhere, so shrink across the 175 mm board cannot bind anything.
   Pegs are loose keepers; no slicer scaling, no shrink gauge.
3. **Heights.** All datums are on one print from the same bed, so first-layer squish and shrink
   cancel between floors and boss tops. Slicer rounding to 0.2 mm layers is why the rear bosses
   sit 0.3 mm low and not less.
4. **Settings.** PETG, 0.4 nozzle, 0.2 mm layers, 3 perimeters, 20 % gyroid, 5 top and 5 bottom
   layers so the mid-print floors are flat, brim, bed about 80 C. Bench face down, no supports,
   nothing bridges. About 150 g and 8 to 10 h; coupon about 1.5 h.
5. **Material.** PETG over PLA. PLA softens near 60 C and the combo's metal shell conducts iron
   heat into its floor.

## 8. Open questions

1. The pocket sizes come from vendor STEP models. The coupon print and the real parts settle them.
2. The lid's rear-plate slot clearances are not yet in the closure spec, so the squareness target
   in need 2 is a judgement. Tighten it when the plate is drawn.
3. JLC's lead trim height is assumed from the 3D models (2.4 mm). Check one assembled board with
   calipers before trusting the 5 mm gap.

## Files

Component `solder-jig` in the Fusion document `cubevox-enclosure`. Export
`hardware/exports/solder-jig.stl`. Renders `hardware/renders/solder-jig-*.png`.
