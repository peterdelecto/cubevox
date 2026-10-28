# Solder jig for the underside jacks

Owner 2026-10-24, rev 4 2026-10-27. JLC assembles the top side. The owner then hand-solders four
through-hole parts that hang from the underside and face the rear wall: combo J101, 1/4" J108,
barrel J106 and the slide switch SWPWR1. This is the plan for the printed jig that holds them while
they are soldered. The jig is its own part. It is not the enclosure dish or lid.

Each part stands in a rectangular pocket cut to the owner's measured part. The pocket's floor
holds the part at the board, its front fence sets yaw, a lip behind keeps it on the fence, and
loose side walls keep it upright. The board drops onto four pegs and the pins find their holes.
Nothing in the jig springs. The whole jig is one printed PETG part. Nothing is added to it, no
foam, no inserts, no screws, no second print, no loose pieces (owner 2026-10-26).

The first build (2026-10-24) rested each part on a hard floor set to the drawing and pushed it
with leaf springs; section 0 lists its four faults. Rev 2 (2026-10-26 and 2026-10-27) replaced
the finger with springs, about 28 features, and the owner deleted it from the Fusion document.
Rev 3 (2026-10-27) had fences and pegs and no floors, so nothing held the parts in the board
(owner 2026-10-27). Rev 4 brings the floors back, cut to the measured parts, inside plain
rectangular pockets. The three part models rebuilt on 2026-10-26 stay (section 4 step 1).

## 0. What the first build got wrong (owner 2026-10-26)

| # | Fault | Cause in the first build |
|---|---|---|
| 1 | Gravity pulls every part off the board | The board sits on top and the parts hang below it. Each part rests on a hard printed floor and the board rests on the parts. Nothing pushes a part up. A floor that prints 0.1 mm low leaves that part 0.1 mm off the board, and the part bodies themselves carry ±0.2 to ±0.3 mm in height, so four hard floors cannot all be right at once. |
| 2 | The leaf springs do nothing, and when they do act they skew the part | Each spring is a 0.8 mm upright finger whose bump reaches 0.3 mm (1/4") or 0.56 mm (barrel, measured in the model) into the pocket. FDM walls land ±0.15 mm, so the bump can print at zero reach and touch nothing. When it does touch, it pushes sideways at one point 5 to 7 mm below the board while the pins hold the part at the board, so the force rolls the part about its long axis and lifts one side of the flange. A cantilever tip also rotates as it bends, about 2.5 deg at 0.3 mm on a 7 mm finger, so the push is never square to the part. |
| 3 | Tolerances too loose for a permanent joint | The pocket walls (0.15 mm datum, 0.5 to 0.6 mm loose) were the only thing setting yaw, and they reference the body sides, which the drawings give as ±0.2 mm. The pins leave the 1/4" about 1.5 deg of yaw and the barrel 2 to 3 deg. A tight wall on a ±0.2 mm body is a bind, not a datum, and a loose one sets nothing. |
| 4 | The barrel model is not the real part | `PJ-002A` in the enclosure document was a plain 14.4 x 9.0 x 11.0 mm box with a bore and three 0.3 mm strips for pins. The CUI drawing (rev 1.05) shows a 3.5 mm front block, a lower box to 6.5 mm with a Ø8.2 half-cylinder cover above it, a rear foot, terminal 1 as a 3.0 mm plate flat on the rear face, and terminal 3 as a 2.5 mm plate flat on the side wall. The 1/4" `PJ-611E` and the switch were built the same way. Section 2 now carries the real geometry from the owner's four drawings. All three models were rebuilt in the Fusion document on 2026-10-26 as timeline features in their existing frames (section 4 step 1). KiCad and the `*_datasheet.step` exports are out of scope (owner 2026-10-26). The step files on disk are the 24 October placeholders and are stale. |

Faults 1 to 3 have one fix. The pocket floors are cut to the owner's measured parts, not the
drawing nominal, and stand 0.1 mm high, so the hand resting on the board clamps every part
between its floor and the board. The front fence sets yaw at zero bias, the lip behind holds the
part on it, and the side walls are 0.5 mm loose so nothing fights the pins. Fault 4 was fixed on
2026-10-26 by rebuilding the three models from the drawings. On 2026-10-27 the owner measured
the four parts, and every figure agrees with its drawing (2.11).

## 1. What the jig must do

| # | Need | Why it matters | Target |
|---|---|---|---|
| 1 | Seat every part flat on the underside with no gap | A gap under a jack moves its mouth out of the back-wall hole and loads the solder joints when a plug goes in | 0 mm gap; each part stands on a floor cut to its measured height and 0.1 mm high, and the hand resting on the board clamps it there while one pin is tacked |
| 2 | Set yaw at the mouth | The back wall has one hole per jack; a skewed jack binds in its hole or shows a crooked mouth. The pins do not set yaw on the 1/4" and barrel | yaw within 0.5 deg, set by the part's own front face on a flat fence, with a lip 0.1 mm behind the part so it cannot leave the fence; pitch comes from the part's feet on the board |
| 3 | Not fight the pins in x and y | Pins in plated holes fix x and y; a jig face that also fixes them binds on print and body error | one fence each on the barrel, 1/4" and switch, its face where the pins put the part's face, zero bias (switch 0.1 mm back); every other pocket face is 0.5 mm loose and the lips 0.1 mm; the pins' slop covers the print error |
| 4 | Hold the board level and still | The owner needs both hands for iron and solder | board on four bosses over four plain pegs, cannot rock or slide; the hand resting on it with the iron presses the board down onto the parts and the parts onto their floors |
| 5 | Keep clear of what is already on the underside | JLC's pot and toggle pins stand about 2.4 mm proud with fillets; test pads and vias are there too | nothing touches the underside except at the four rest seats |
| 6 | Leave every pin reachable from the top | Soldering happens on the top side between the rear pot row and the edge | nothing of the jig rises above the board but the peg heads; the rear edge is open |
| 7 | Take the heat | Iron tip 350 C, pins conduct into the part body, PETG softens at about 80 C | no jig wall within 1 mm of a pin exit or a terminal; the fences touch plastic only; the barrel fence spans the bore mouth with a Ø4 x 1 mm relief that keeps it 1 mm off the centre pin; the barrel lips stand 1 mm clear of terminal 1 and the switch rear wall 1 mm under its terminals |
| 8 | Let the parts load and unload by hand | The owner does several boards | parts drop into their pockets mouth first, the board drops onto the pegs and is wiggled until the pins find their holes, and the board lifts off with the parts on it |
| 9 | Be one printed part, printable at home in one go | One piece, no supports, PETG; nothing added | plain walls, a rear bar, bosses, pegs, fences, pocket walls, lips and ribs, every one a rectangle or a circle in plan; a pocket is a rectangle around the part's envelope, never an outline of the part; floors are ribs, never a block or a slab; no bridges, no overhangs past a 45 deg chamfer; flat bench face down, fits a 220 x 220 bed |
| 10 | Print once and work | The jig is printed, so no printed face may be a tight fit | every gap the part must drop through is loose; the two datums are a fence the part is led onto and a floor cut to the measured part; a floor that proves wrong moves by one parameter and the jig is reprinted |

## 2. Fixed facts from the board and the drawings

Sources are the owner's four drawings (2026-10-26): CUI PJ-002A rev 1.05, G-Switch SS-12D11-G050,
Neutrik NCJ6FA-H, HOOYA PJ-611E rev A2. Depths below are measured from the board's underside into
the part, so "25 mm deep" means the far face is 25 mm below the board. Undimensioned values are the
owner's scaled readings, good to about ±0.1 mm. On 2026-10-27 the owner measured the four parts
with calipers; every height, length and width agrees with its drawing, so the figures below are
the measured figures (2.11).

1. Board 185 x 145 mm, 1.6 mm thick, rear edge at the top of the KiCad view. Four plain rest
   seats with 3.2 mm holes at (5, 5), (180, 5), (20.5, 140), (164.5, 140) mm from the rear-left
   corner. They carry no screws, so the jig may use them.
2. **Barrel PJ-002A.** 9.0 wide, 11.0 deep, 14.4 long. Front block 3.5 long, full 9 x 11, with a
   slot across its far face. Behind it a lower box 9 wide and 6.5 deep from 3.5 to 13.5, and
   above that a Ø8.2 half-cylinder cover on the bore axis, 10.6 deep. Rear foot from 13.5 to
   14.4, 2.5 deep, full width, with a 3.0 slot for terminal 1. Bore Ø6.5 on the centre line,
   6.5 below the board. Terminal 1 is a 3.0 x 0.36 plate flat on the rear face, reaching 8.55
   deep; terminal 3 is a 2.5 x 0.40 plate flat on the right side wall at 10.7 from the front,
   reaching 3.0 deep. Terminals 1 and 2 sit on the centre line, so the body is centred on its
   pins. Bodies ±0.2, pin positions ±0.1. The front plate is 3.7 mm past the board edge, so the
   front block sits 0.2 to 3.7 mm past it. The side ledges at 6.5 mm deep, where the round cover
   starts, are only 0.4 mm wide. The drawing does not place the Ø2.0 centre pin's tip; the
   model has it flush with the front plate (8.9).
3. **1/4" PJ-611E.** Case 15.8 wide, 12.6 deep, 21.4 long plus a Ø8.5 to Ø7.3 rear cap to 24.4.
   The axis is 6.5 below the board and 1.4 off the case centre toward the 3#/5#/7# side. The
   case touches the board on two feet only, the front rib (0 to 1.85 from the front face, full
   outline) and the rear part of the rear rib (20.45 to 21.39); between them the underside is
   1.2 off the board. Two middle ribs 1.5 thick at 6.74 and 13.15 and the rear rib at 19.54 run
   the full 15.8 x 12.6 outline; the case between ribs is 1.0 in from each rib face, and the
   3#/5#/7# side is open above 6.48 deep in each bay. The front face carries the bushing's
   10 x 10 flange 0.4 proud, beads to 0.61 proud at 4.15 from the axis, and a tab of unknown
   depth 4.84 to 7.40 toward the 2#/4#/6# side between 2.81 and 4.71 deep; below 1.4 deep the
   front face is clean across its full width. Bushing Ø9.28 crest by the drawing, Ø9.52 in the
   model (a 3/8" thread, 8.10), 8.7 past the front face, which is 0.3 inside the board edge.
   Six lugs 0.5 thick, 1.2 to 1.4 wide, in 1.8 x 0.9 slots.
4. **Switch SS-12D11-G050.** Right-angle slide switch. Body 12.7 wide across its cover plates by
   the owner's scaled reading (the drawing shows 13.2 across the cover legs; see 8.6), 13.4
   across the side flaps (flaps run only 2.0 back from the front face), 6.6 deep, 6.4 long from
   the front face to the case rear; three terminals run back at mid-depth to 10.4 and bend
   down through the board 8.4 behind the two cover legs. Lever 3.9 square, 5.0 past the front
   face, centred at 3.3 deep. Front face 0.5 inside the board edge. Five round pins hold yaw.
5. **Combo NCJ6FA-H.** Read from the Neutrik vendor STEP in the Fusion document. Lengths along
   the housing are measured behind the shoulder face, which is 0.5 mm inside the board edge. The
   front block with the ears is 25 mm wide and 6.3 mm long and is the only part of the housing
   that touches the board. Behind the front block the housing's top is flat, 1.8 to 3.3 mm below
   the board. The axis is 12.6 mm below the board and sits 0.7 mm to the left of the footprint
   origin, toward the 1/4" jack (x 118.33 in board coordinates). From 6.3 to 11.4 mm behind the
   shoulder face the housing is a section of radius about 10.9 mm on the axis. From 11.4 to
   18.0 mm (11.8 to 18.4 mm behind the board edge) its sides are a cylinder of radius 8.6 mm on
   the axis, with flats 8.45 mm either side of the axis from 11 to 14.2 mm deep. Below the
   cylinder a keel runs from 21 to 25.1 mm deep. The keel is 12.8 mm wide at its widest, 22.6 mm
   deep (x 111.93 to 124.73 in board coordinates). The radius 12.5 mm surface is the keel's
   bottom strip (x 113.46 to 123.2) and does not form the housing's sides. The housing ends
   24.5 mm behind the shoulder face. The PUSH tab reaches 29.0 mm deep and stands 7.0 to 7.7 mm
   ahead of the shoulder face (6.5 to 7.2 mm past the board edge), 16 mm above the 45 mm bench.
   Eleven pins over 22.9 mm in Ø1.6 and Ø1.2 holes and two pegs hold yaw and pitch.
6. Everything else on the underside is pin stubs, about 2.4 mm, over the pot and toggle rows.
7. The 1/4" jack ships with a nut and gasket on its bushing; both come off before loading.
8. Hole slop per part.

   | Part | Pins | Slop in x | Slop in y (toward the mouth) | Yaw left by the pins |
   |---|---|---|---|---|
   | 1/4" | six lugs 0.5 x 1.3 in 1.8 x 0.9 slots, 12.8 x 11.2 apart | ±0.25 | ±0.2 | about 1.5 deg |
   | barrel | tabs 0.25 to 0.40 thick, 2.5 to 3.0 wide, in 1.0 x 3.0 to 3.5 slots | ±0.3 | ±0.25 | 2 to 3 deg |
   | switch | two legs 0.2 x 1.2, three terminals 0.7 x 1.3, round holes | ±0.1 | ±0.1 | held |
   | combo | eleven pins over 22.9 mm, two pegs | ±0.1 | ±0.1 | held |

9. Part weights. Combo about 30 g, 1/4" about 10 g, barrel about 3 g, switch about 1 g. The
   assembled board is about 300 g.
10. PETG as printed. Modulus about 2000 MPa. An upright wall t thick, w wide and L tall,
    pushed at its top, deflects there with stiffness 3 x 2000 x (w t³ / 12) / L³ N/mm. A 1/4"
    post 6 thick, 2.4 wide and 12 free gives about 150 N/mm. The barrel fence, 6 thick, 9 wide
    and 36.5 free, gives about 20 N/mm. A 2 N push moves either under 0.1 mm.
11. Measured parts (owner 2026-10-27, calipers). Height is from the board face to the lowest
    point, length from the front face to the face the lip bears on, width across the widest part
    the pocket walls see. Every figure agrees with its drawing.

    | Part | Height | Length | Width |
    |---|---|---|---|
    | barrel | 11.0 at the front block | 13.5 to the lower box's rear face; the foot ends at 14.4 | 9.0 |
    | 1/4" | 12.6 at the ribs | 21.4 to the case rear beside the cap | 15.8 |
    | switch | 6.6 | 6.4 to the case rear | 13.4 over the flaps |
    | combo | 25.1 at the keel strip | 24.5 behind the shoulder face | 25.0 over the front block |

    The Fusion models differ from two of these in ways that matter to the jig. The combo model is
    25.085 deep, 0.015 under the measurement, and its keel strip is at full depth only over
    14 mm, from 6.84 to 20.84 behind the shoulder face; behind that the model steps up to 23.3.
    Its 25.0 width is two ears at diagonal corners, and the top-right ear is only 2.7 deep. The
    switch model's terminals run 1.8 to 2.0 deep, not the 2.65 to 3.95 read from the drawing in
    2.4.

## 3. Design (rev 4)

1. **Orientation.** Board face up, parts hanging below, soldering from the top. The other way
   round hides the pins.
2. **A pocket per part.** Each part stands in a rectangular pocket cut to its measured envelope
   (2.11). The pocket has four kinds of face and each does one job. The floor holds the part at
   the board. The fence sets yaw. The lip behind keeps the part on the fence. The side walls
   keep it upright and are loose. Every pocket face is a rectangle in plan; nothing follows the
   part's shape.
3. **Floors.** Each floor's top sits `jigFloorBias` 0.1 mm nearer the board than the part is
   deep, so the part's feet reach the board 0.1 mm before the board reaches its bosses, and the
   hand resting on the board clamps the part (need 1). The floor depths are the measured
   heights, `jigFloorBarrel` 11.0, `jigFloorQuarter` 12.6, `jigFloorSwitch` 6.6 and
   `jigFloorCombo` 25.1. A floor is ribs `jigRib` 2 mm thick standing from the bench, never a
   block or a slab (need 9).

   The barrel floor is one block under the front block only, wall to wall and 3.5 mm long from
   the fence face, because the front block is the barrel's only face at 11.0 mm (2.2); it
   stands on the rear bar. The 1/4" floor is one rib along the case centre line for the case's
   length and two across it, wall to wall, under the front rib and the rear rib, so the case's
   four ribs rest on them. The switch floor is two ribs across, wall to wall, under the front
   and rear of the case. The combo floor is one rib along the axis under the keel's bottom strip
   for the 14 mm it is at full depth (2.11) and two across at its ends, wall to wall; the strip
   is a radius 12.5 mm surface, so the combo rests on a line and its side walls keep it from
   rolling. Where a rib would leave a slot under 2 mm against a fence, lip or rear wall, it
   runs on to meet it, so the switch front rib starts at the fence face, the switch rear rib
   runs back to the rear wall (2.5 mm wide) and the 1/4" rear across rib and along rib run back
   to the lip faces (2.1 mm wide); every rib still covers its part for the full `jigRib`.
4. **Fences.** As rev 3, zero bias, `jigFence` 6 mm thick, standing on the rear bar. The 1/4"
   fence is the lower wall to 12.5 mm under the board, 1 mm below the flange, with two posts to
   0.5 mm under it, each 0.3 mm off the flange or the tab and 1 mm past the case edge. The
   barrel fence runs to 0.5 mm under the board with the Ø4 x 1 mm centre pin relief, and widens
   from 9 to 18.25 mm so the pocket walls meet it and no slot is left to the switch post; its
   left side lies on the outer face of the wall the barrel and switch pockets share. The switch
   fence is the two posts either side of the lever, faces `jigSwitchGap` 0.1 mm back, to 1.0 mm
   under the board, 1.19 mm from the cover legs; it is on, as the switch pocket's front. The
   combo has no fence; its eleven pins hold yaw to about 0.25 deg and there is 0.5 mm beside
   its nose.

   `jigFenceGap` stays 0 (owner 2026-10-27). The pins' slop of 0.2 to 0.25 mm toward the mouth
   (2.8) covers a fence printed 0.15 mm back. A fence 0.1 mm into the part plus 0.15 mm of
   print error would exceed that slop and bind (need 3). The switch's pins have only ±0.1 mm,
   so its fence sits 0.1 mm back.
5. **Lips.** Behind the barrel and the 1/4" a lip stands `jigLipGap` 0.1 mm behind the measured
   rear face, `jigWall` 3 mm thick, from the bench to 4 mm under the board. The part can sit at
   most 0.1 mm off its fence, 0.6 deg on the barrel's 9 mm and 0.4 deg on the 1/4"'s 15.8 mm,
   and only if something pushes it back.

   The barrel's lip is two posts, each from a side wall inward to 2.5 mm off the axis. They bear
   below their chamfer on the lower box's rear face, 13.5 mm from the front plate, and on the
   round cover's rear face under it, down to about 9.7 mm deep. The 5 mm between them keeps 1 mm
   clear of terminal 1, 3.0 mm wide on the axis (2.2), and the rear foot, 2.5 mm deep, passes
   1.5 mm above them (need 7). The 1/4"'s lip is two posts on the rear rib's back face at
   21.4 mm, each from a side wall inward to 1 mm short of the Ø8.5 cap, 1.75 mm wide on the
   3#/5#/7# side and 4.55 mm on the other.

   The switch gets a plain rear wall `jigGapLoose` 0.5 mm behind the case, to 5.0 mm under the
   board, 1 mm below its terminals at 2.65 to 3.95 mm deep (2.4); its pins hold it to ±0.1 mm,
   so the wall only keeps it in the pocket. The combo gets a plain rear wall 0.5 mm behind the
   housing end, to 4 mm under the board, for the same reason.
6. **Side walls.** `jigWall` 3 mm thick, inner faces `jigGapLoose` 0.5 mm off the measured width
   (barrel 9.0, 1/4" 15.8, switch 13.4 over the flaps, combo 0.5 off the housing's widest face
   below the wall top on each side, because its 25.0 mm ears sit above the walls, 2.11). They
   run from the fence face back to the lip's or rear wall's back face, and for the combo from
   the shoulder face. They stand from the bench to 4 mm under the board, below the 2.4 mm pin
   stubs (2.6) and 1 mm below the barrel's terminal 3, which reaches 3.0 mm deep on the side
   wall (2.2). They merge with the fence, the lips, the ribs and the bosses where they meet, and
   no slot narrower than 2 mm is left anywhere in the body. Where two pockets' walls would
   overlap or come within 3 mm, the pockets share one wall; the barrel and switch share one,
   5.25 mm thick.

   As built, the combo's walls are 24.456 mm apart. The left wall is 0.5 mm off the bottom-left
   ear, and the right wall 0.5 mm off the front block's round body, which is the widest face
   below the wall top on that side; the top-right ear's lower edge is 4.9 mm deep, inside the
   lead-in band, and the chamfer stands 0.84 mm off it. The 1/4" left wall is 3.1 mm thick
   where its outer face runs tangent to the rear-left boss. A rectangular web at each rear
   corner, bounded by the frame wall, the boss, the rear bar and the nearest pocket wall, fills
   the 1 to 1.85 mm gaps that would otherwise be left between them; both webs are part of
   `pocket walls`.
7. **Lead-in.** A 45 deg chamfer `jigLeadIn` 2 mm runs along the top inner edge of every pocket
   side wall and of the barrel and 1/4" lips, not the fences and not the two loose rear walls.
   A part dropped in up to 2 mm back lands its rear bottom corner on the lip's chamfer and
   slides forward onto its fence before it seats (need 2). The chamfer ends 6 mm under the
   board, 0.5 mm above the barrel's lower box corner at 6.5 mm and 6.6 mm above the 1/4"'s rib
   corners, so the vertical face below it is what the seated part bears on. Where a side wall's
   chamfer passes over the switch rear wall, which stops 1 mm lower, the chamfer cuts only
   above that wall's top and the wall stays whole.
8. **Frame, seats and pegs.** As rev 3. Two side walls and a front wall `jigWall` 3 mm thick
   stand under the board's edge strips to 4 mm under it. The rear bar `jigBar` 8 mm tall and
   10.2 mm wide, in y from 0.5 mm inside the board edge to 9.7 mm past it, carries the fences;
   its top stays 8 mm below the combo's PUSH tab (2.5). The bench `jigBench` is 45 mm under the
   board, kept from rev 3 so the bar clears the PUSH tab and nothing in the frame moves. Four
   bosses `jigBoss` 7 mm at the rest holes (2.1) rise to the board underside and merge into the
   walls; each carries a plain peg `jigPeg` Ø3.0 mm in the 3.2 mm hole, `jigPegHeight` 4 mm,
   0.5 mm chamfer, about ±0.1 mm play (owner 2026-10-27). No snap pegs. Shrink is not modelled;
   the slicer compensates.
9. **Access.** Nothing rises above the board but the four peg heads. Pocket walls stop 4 mm
   under it, so the top 4 mm of every part stands clear above its pocket, and the fences stop
   0.5 mm under it (switch 1.0 mm). Nothing needs a finger; the hand works on the board (need 6).
10. **Parameters**, in mm. `jigBench` 45, `jigWall` 3, `jigBar` 8, `jigFence` 6, `jigFenceGap`
    0, `jigSwitchGap` 0.1, `jigPeg` 3.0, `jigPegHeight` 4, `jigBoss` 7, and new for rev 4
    `jigGapLoose` 0.5, `jigLipGap` 0.1, `jigFloorBias` 0.1, `jigFloorBarrel` 11.0,
    `jigFloorQuarter` 12.6, `jigFloorSwitch` 6.6, `jigFloorCombo` 25.1, `jigRib` 2 and
    `jigLeadIn` 2. Eighteen in all.
11. **Volume.** 123.8 cm³ as built, the rev 3 frame at 84 cm³ plus 40 cm³ of pocket walls, lips,
    ribs and corner webs.

## 4. Build steps in Fusion (rev 4, built 2026-10-27)

1. **Part models (done 2026-10-26).** `PJ-002A`, `PJ-611E` and `SS-12D11-G050` were rebuilt in
   the Fusion document from the drawings in section 2 as timeline features in their existing
   frames. `PJ-002A` has the front block 9 x 11 mm for 3.5 mm, sides 6.5 mm deep behind it with a
   Ø8.2 mm round cover crowning at 10.6 mm, the rear foot 2.5 mm deep from 13.5 to 14.4 mm with a
   3.0 mm slot, the terminal 1 plate 3.0 x 0.36 mm up the rear to 8.55 mm, the terminal 3 plate
   2.5 x 0.40 mm on the side wall to 3.0 mm deep, and the bore and centre pin. `PJ-611E` stands on
   two feet, the front rib from 0 to 1.85 mm and the rear foot from 20.45 to 21.39 mm, with a
   1.2 mm gap under everything between them. Its bays, 4.71 to 8.85, -1.70 to 3.21 and -8.09 to
   -3.20 mm along the part's own x', are recessed 1.0 mm on both sides and the far face. The ribs
   are the full 15.8 x 12.6 mm. It carries a Ø8.5 mm cap 3.0 mm long on the axis, the
   10 x 10 x 0.4 mm flange, and a 1.9 x 2.56 mm tab modelled 1.0 mm proud on the front face. The
   washer and nut moved out 0.4 mm to sit on the flange. `SS-12D11-G050` has a case 12.7 mm wide
   behind the first 2.0 mm and flaps 13.4 mm wide. Two simplifications stand. The 1/4" open side
   (3#/5#/7#, above 6.48 mm deep in each bay) is the 1.0 mm recess only, because the drawing does
   not show the interior (8.7). The switch's 12.7 mm width is the owner's scaled reading; the
   drawing shows 13.4 mm over the flaps and 13.2 mm across the cover legs (8.6). KiCad and the
   `*_datasheet.step` exports are out of scope (owner 2026-10-26). The step files on disk are the
   24 October placeholders and are stale. The fence and posts are drawn from these faces.
2. The component `solder-jig` built for rev 3 on 2026-10-27 is reworked in place, not rebuilt.
   Its two planes `board underside` and `jig bench`, `jigBench` below it, stay.
3. `bench sketch` on the bench plane keeps the rev 3 walls, rear bar, bosses, barrel fence,
   switch fence posts and the 1/4" fence as three profiles (the two post rectangles inside the
   lower wall rectangle), and gains the pockets: per pocket two side wall rectangles, the lip
   posts (barrel two, 1/4" two) or the rear wall (switch, combo), and the floor ribs (barrel one
   block, 1/4" three ribs, switch two, combo three) and the two rear corner webs. The barrel
   fence rectangle widens to 18.25 mm. Every edge is dimensioned to projected board or part
   geometry or to a parameter, and the sketch is fully constrained. As built that is 34
   rectangles and 4 circles on 57 dimensions and nothing else. A wall, fence, lip, rib or web
   is a rectangle, a boss or peg is a circle. If a shape needs more than four sides, the design
   is wrong, not the sketch.
4. The rev 3 extrudes `walls`, `rear bar`, `bosses`, `1/4" lower wall` and `1/4" posts and
   barrel fence` stay, and `switch fence` is unsuppressed. New extrudes from the same sketch,
   one per height: `pocket walls` takes every side wall, both lips, the combo rear wall and the
   two corner webs to 4 mm under the board; `switch rear wall` to 5 mm under it; `barrel floor`,
   `1/4" floor`, `switch floor` and `combo floor` each rise `jigBench - jigFloor<part> +
   jigFloorBias` from the bench.
5. `lead-in chamfer`, `jigLeadIn` 2 mm at 45 deg, on the top inner edges of the pocket side
   walls and of the barrel and 1/4" lip posts (3.7).
6. `pegs sketch` on `board underside` with its four `jigPeg` circles, `pegs`, `peg chamfer`,
   `barrel pin relief sketch` and `barrel pin relief` stay as rev 3. A sketch on a face projects
   the face's outline automatically; that outline is removed before a cut, or the cut takes the
   whole face.
7. As built: one body `solder jig`, 145 faces, 123,844 mm³; three sketches on two planes, every
   sketch fully constrained; sixteen features, none suppressed; 183 timeline items, none
   unhealthy; the eighteen parameters of 3.10 and no others. Every sketch, plane, feature and
   parameter is named. A sketch edit after a chamfer loses the chamfer's edge references; the
   chamfer is the last feature, so it is deleted and recreated rather than repaired.

## 5. Verification

1. **Model.** The jig is one body. Interference over the jig, the board and the four parts lists
   only each floor against its own part, 0.1 mm deep by design (3.3), and the 1/4" washer and
   nut against the left post, which come off before loading (2.7). The fences touch their parts
   at 0 and the boss tops touch the board. No jig face stands within 1 mm of a pin or a
   terminal. Nothing rises above the board but the peg heads.

   Measured on the build of 2026-10-27, in mm, by the agent's script; the lead's own read-only
   audit confirmed the counts, the parameters, the interference set and the touching distances.
   Interference over the jig, the board and the four parts lists each floor in its own part
   (barrel 3.15 mm³, 1/4" 6.81 mm³, switch 5.09 mm³, combo 2.09 mm³), the washer and nut
   against the left post, and nothing with the board.

   | Check | Target | Measured |
   |---|---|---|
   | 1/4" fence and barrel fence to the front faces | 0 | 0.000, 0.000 |
   | barrel fence to the centre pin, tip and side | at least 1.0 | 1.000, 1.000 |
   | switch posts to the front face, lever sides, cover legs | 0.1, 0.5, at least 1.0 | 0.100; 0.500, 0.500; 1.193 |
   | posts to flange and tab | 0.3 | 0.300, 0.300 |
   | lower wall to flange bottom | 1.0 | 1.000 |
   | floor tops into their parts, four parts | 0.1 each | 0.100, 0.100, 0.100; combo 0.085 (model 0.015 shallow, 2.11) |
   | barrel lips to the lower box rear face | 0.1 | 0.100 |
   | 1/4" lips to the rear rib's back face | 0.1 | 0.100 |
   | barrel lips to terminal 1 | 1.0 | 1.000 |
   | barrel lips to the rear foot | 1.5 | 1.921 |
   | 1/4" lips to the cap | 1.0 | 1.000 |
   | switch rear wall to its terminals | at least 1.0 | 3.000 (model terminals at 1.8 to 2.0 deep, 2.11) |
   | switch and combo rear walls to their parts | 0.5 | 0.500, 0.500 |
   | side walls to their parts, each side | 0.5 | 0.500 on all eight sides |
   | side walls to the barrel's terminal 3 | 1.0 | 2.192 |
   | pocket walls and lips to board underside | 4.0 (switch rear wall 5.0) | 4.000; 5.000 |
   | frame walls to board underside | 4.0 | 4.000 |
   | fences to board underside | 0.5 (switch 1.0) | 0.500 at 3 faces; 1.000 at 2 |
   | boss tops to board underside | 0 | 0.000 at 4 |
   | rear bar top to the PUSH tab | 8.0 | 8.002 |
   | nearest jig face to any pin or terminal | at least 1.0 | 1.000 |
   | pegs to hole walls | 0.1 | 0.100 at 4 |
   | jig faces above the board | peg heads only | peg tops and chamfers only |
   | jig size | fits 220 x 220 | 185.0 x 154.7 x 49.0 |
   | gaps under 2 mm between features in plan | none | none; five closed by the webs, the tangent wall and the wider barrel fence |
   | volume | about 125 cm³ | 123.8 cm³ |
2. **First print.** Each part drops into its pocket by hand and lifts out; a part that needs a
   push has a pocket printed short, and `jigLipGap` opens by 0.1 mm. The board drops onto the
   pegs and lifts off; a peg that needs a push printed oversize, and `jigPeg` drops 0.1 mm. With
   the board pressed by hand, a lamp behind shows no light under any flange and every mouth
   lies on its fence end to end. Light under one flange means that floor printed low; that
   part's floor depth parameter loses 0.1 mm, which raises the floor. A board corner standing
   more than 0.3 mm off its boss means a floor printed high; that parameter gains 0.1 mm. A part
   that sits high on its pins means a fence printed too far forward; `jigFenceGap` opens to
   0.1 mm. Every change is a reprint.

## 6. Use

1. The nut and gasket come off the 1/4" jack.
2. Each part goes into its pocket mouth first, front face to the fence, and down onto its floor.
   The chamfers lead it on.
3. The board goes onto the pegs and is wiggled down until every pin finds its hole. The pegs
   take the board first, then the pins.
4. A lamp behind the jig shows every mouth from the rear: level, parallel, on its fence, with no
   light under any flange.
5. The hand rests on the board over each part in turn and one pin of that part is tacked from
   the top. Then every pin is soldered.
6. The board lifts off the pegs with the parts on it.

## 7. Printing

1. **Shape.** PETG, bench face down, no supports, no bridges. The walls, bar, bosses, fences,
   pocket walls, lips, ribs and pegs all rise from the bed or from solid below; no floor is a
   slab between walls. The only overhangs are the 45 deg lead-in and peg chamfers, which print
   without support. The rear bar reaches 9.7 mm past the board edge, so the jig is about
   185 x 155 mm and fits a 220 x 220 mm bed. About 125 cm³ of PETG (3.11).
2. **Slicer settings are not in this spec.** Layer height, perimeters, brim, temperatures and
   shrink compensation belong to the slicer, not the model (owner 2026-10-27). The jig is drawn
   at nominal size. Pins set each part's x and y against the board, not the jig, and the pegs
   sit 0.2 mm loose in their holes.
3. **Nothing bought.** The jig is the only part.
4. **Material assumed.** PETG. The 1 mm rule in need 7 assumes it softens near 80 C; PLA near
   60 C would need more. The fences touch plastic only.

## 8. Open questions

1. Resolved 2026-10-27. The owner set the rear-wall slot clearance at 0.25 mm per side around
   each jack, which keeps the 0.5 deg yaw target in need 2. The enclosure spec does not carry
   the number yet; it goes in when the lid's rear plate is drawn.
2. JLC's lead trim height is assumed from the 3D models (2.4 mm). Check one assembled board with
   calipers before trusting the 4 mm gap above the walls.
3. The holes in the back wall were positioned from the placeholder models. They must be
   re-checked against the rebuilt parts and the combo numbers in 2.5 before the back wall is
   printed. The 1/4" bushing is 1.4 mm off the case centre and 6.5 mm below the board, the barrel
   bore is centred and 6.5 mm below, and the combo axis is 12.6 mm below the board and 0.7 mm left
   of the footprint origin, toward the 1/4" jack (x 118.33 in board coordinates). From 11.4 to
   18.0 mm behind its shoulder face the combo housing is the radius 8.6 mm cylinder with a keel
   below it; the radius 12.5 mm surface is only the keel's bottom strip (2.5).
4. Resolved in rev 4. No finger reaches under the board; the hand works on it.
5. The 1/4" drawing gives no figure for how far the tab stands off the front face. The rebuilt
   model carries it 1.0 mm proud until measured. The 2#/4#/6# post stands beside the tab, between
   it and the case edge, so the post clears it however far it stands.
6. The switch's 12.7 mm case width is the owner's scaled reading. The drawing shows 13.4 mm over
   the flaps and 13.2 mm across the cover legs. The model uses 12.7 mm. The owner's calipers
   read 13.4 mm over the flaps (2.11), and the switch pocket's side walls are 0.5 mm off that.
7. The 1/4" model shows the open 3#/5#/7# side (above 6.48 mm deep in each bay) as the 1.0 mm
   recess only, because the drawing does not show the interior. The model is an envelope there
   and does not show what the open side exposes.
8. Resolved in rev 4. The switch is pocketed and its fence is on (3.4). The combo keeps its pins
   for yaw; the first board shows whether they hold 0.5 deg.
9. The CUI drawing does not place the barrel's centre pin tip, and the model has it flush with
   the front plate. The fence carries a Ø4 x 1 mm relief so it never touches the pin either
   way. A real part in hand settles it; if the tip sits back more than 1 mm the relief can go.
10. The 1/4" bushing is Ø9.28 in the drawing and Ø9.52 in the model. The posts clear the
    model's bushing by 0.54 mm and the flange sets the 0.3 mm gap, so the jig is unaffected.
    The board body in the Fusion document is 1.516 mm thick against the 1.6 mm in 2.1; the jig
    references the board's underside face, so that does not move anything either.
11. The floors stand 0.1 mm high (3.3) so that the hand on the board clamps all four parts. The
    first print shows whether a 1.6 mm board pressed by hand closes onto four floors at once. If
    one part stays proud, its floor parameter gains 0.1 mm (5.2).
12. The barrel's lips bear on the lower box's rear face at 13.5 mm and the round cover below it
    (3.5). The drawing shows the cover ending flush with the box; if the real cover stops short,
    the lips bear on the box alone between 6 and 6.5 mm deep, and `jigLeadIn` drops to 1 mm to
    give them more face.

## Files

Component `solder-jig` in the Fusion document `cubevox-enclosure`, new for rev 3 because the owner
deleted the old one on 2026-10-27 and reworked in place for rev 4; the owner prints from Fusion,
no STL in the repo. Part
components `PJ-611E`, `PJ-002A` and `SS-12D11-G050` in the same document, rebuilt from the
drawings as timeline features on 2026-10-26. KiCad and the
`hardware/cubevox.3dshapes/*_datasheet.step` exports are out of scope (owner 2026-10-26). The
step files on disk are the 24 October placeholders and are stale. Renders go to
`hardware/renders/solder-jig-*.png`. Drawings used for section 2 are the owner's four PDFs of
2026-10-26 (CUI PJ-002A rev 1.05, G-Switch SS-12D11-G050, Neutrik NCJ6FA-H, HOOYA PJ-611E rev A2),
not yet copied into the repo.
