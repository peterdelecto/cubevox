# OLED adapter

Owner 2026-10-27. The 1.3" SH1106 module plugs into the socket J117 and was to stand on four
M3 x 9 mm standoffs through its holes and the board's. On the real module the holes sit about
1 mm north of the board's holes, toward the rear edge, so a standoff cannot pass both. A printed
adapter carries both hole patterns: it locates on the board's holes and carries posts for the
module's holes, slid north by one parameter the owner adjusts after printing. The DrumSynth
adapter (Fusion document `DrumSynth enclosure - Faceplate rework`, component `OLED adapter`) is
the model for the construction, not for any dimension; its module is the 1.54".

## 1. What the adapter must do

| # | Need | Target |
|---|---|---|
| 1 | Hold the module at the height the socket wants | module PCB underside `oledSeat` 8.0 mm above the board (owner 2026-10-28; was 9.0, then 8.5); by the drawing's 8.5 mm pins the tips would reach 0.5 below the board top, so the real pins must be shorter than drawn for the module to reach the seats |
| 2 | Put the module's holes where the module's holes are | posts on the board's 30.4 x 28.5 grid slid `oledOffset` 1.0 mm toward the rear edge; `oledOffset` is a user parameter |
| 3 | Hold the module without screws | split pins drawn at the hole diameter in the module's Ø3.0 holes, 0.5 slot (owner 2026-10-30, printed, fits and holds) |
| 4 | Stay on the board by itself | split pins drawn at the hole diameter in the board's four Ø3.2 holes, 0.5 slot (owner 2026-10-30); nothing within 1 mm of the socket or the module's back-side parts |
| 5 | Be one printed PETG part | circles and rectangles in plan, extrudes and tapers only; the frame stands on columns both ways, so it prints with supports from the bed under the frame (6) |
| 6 | Fit on the first print | every fit carries the printer's swell (2.7); no feature under the nozzle width; no reprint to calibrate (owner 2026-10-28) |
| 7 | Be adjustable by parameter | every figure in section 3.6 is a user parameter |

## 2. Fixed facts

Board coordinates: x from the left edge, y from the rear edge, as in the KiCad view. In the Fusion
document `cubevox-enclosure` the board frame has the rear edge at y 0 and y running negative
toward the front, so "toward the rear edge" is +y there.

1. Board holes for the module: four, Ø3.2 non-plated, at (64.58, 28.25), (94.98, 28.25),
   (64.58, 56.75), (94.98, 56.75). Grid 30.4 x 28.5. Read from the Fusion board body and the
   footprint `OLED-1.3-SH1106-I2C-4P`.
2. Socket J117, hanxia 5 mm SMD 1x4, pin row at y 27.75, pins at x 75.98, 78.52, 81.06, 83.60
   (2.54 pitch). The pin row lies 0.5 mm north of the board's north hole row.
3. Module by the vendor drawing (`hardware/tools/model_oled_sh1106.py`): PCB 35.4 x 33.5 x 1.2,
   holes Ø3.0 on the same 30.4 x 28.5 grid, north edge 2.0 mm north of the pin row, glass
   34.5 x 23.0 x 1.3 centred on the PCB and 0.2 mm above it, header body 2.5 mm tall under the
   PCB at the pin row, pins no more than 8.5 mm below the PCB.
   Measured on board 1 with the printed adapter (owner 2026-10-30): the top of the glass sits
   exactly 11.75 mm above the top of the main PCB. The drawing's stack (seat 8.0 + PCB 1.2 +
   gap 0.2 + glass 1.3) gave 10.7 mm, so the real module stands 1.05 mm higher than the drawing.
   The model (`hardware/tools/model_oled_sh1106.py`, the STEP in `cubevox.3dshapes`) now sits on
   the 8.0 mm seat with PCB 1.6, tape 0.65 and glass 1.5, which lands the glass top at 11.75 mm;
   only the total is measured, the split is the usual make-up of these modules. The enclosure
   takes the measured 11.75 mm, 0.05 mm under the 11.8 mm panel plane. In the Fusion document
   (2026-10-30) the regenerated STEP is inserted at the root as `OLED-1.3-SH1106-I2C-4P (2)`,
   origin on the main PCB's top face at XY (79.785, -41.5), the footprint's own position, so
   its PCB rests on the 8.0 seat, its glass top reads 11.75 above the board and its glass runs
   30.0 to 53.0 mm from the board's rear edge (owner 2026-10-30: the module sits 1 mm north of
   where it had been placed by hand, because of how the header was mounted; the lit area starts
   about 30 mm from the rear edge). The earlier hand-placed occurrence `(1)`, at y -42.5 with
   its glass raised to 11.78 on the drawing's stack and its PCB floating 1.08 above the seat,
   is hidden, not deleted, in case the lid work references it.
4. Observed (owner 2026-10-27): with the module plugged in, its holes sit about 1 mm north of
   the board's holes. The drawing's hole position is wrong by that amount; the grid itself is
   not in question.
5. The Fusion module model carries the drawing's holes, on the board's grid, so it does not
   show the offset.
6. The board underside under the module is bare. The nearest underside part is the combo at
   x 105 to 131.
7. Printer allowances, 0.4 mm nozzle, PETG: a small column prints 0.1 to 0.2 mm over its drawn
   diameter; a hole prints under; the layer on the bed flares up to 0.3 mm outward; nothing
   narrower than 0.4 mm prints as drawn. The first adapter print (2026-10-28) showed it: crush
   ribs 0.25 to 0.35 mm proud of their cores printed as one solid column at the crest diameter
   and entered no hole.

## 3. Design

1. **Seats.** Four round bosses `oledBoss` Ø8 mm centred on the board's four holes, from the
   board top to `oledSeat` 8.0 mm. The module's PCB rests on their tops. Each boss also carries
   the post, which sits `oledOffset` off the boss centre; at 1.0 mm the post edge reaches
   2.25 mm from the boss centre, inside the Ø8.
2. **Posts.** On each boss top a split pin for the module hole: `oledPostCore` Ø3.0, the hole's
   own diameter, `oledPostShank` 2.5 tall, slot `oledPegSlot` 0.5 east to west through the pin
   and `oledSlotRise` 1.0 down into the boss, halves 1.25 thick. The print's swell (2.7) makes
   the pin a press in the hole and the slot lets the halves close by that much; the owner set
   these values by print (2026-10-30) and the module presses on and holds. The post top stands
   1.3 above the module PCB and 0.2 under the glass top; the holes lie outside the glass in y.
   A barb feature `post barbs` (Ø3.2 tapering over 0.65 to Ø2.55) remains in the timeline and
   fails: with the shank at Ø3.0 its tapered ring is 0.1 wide and collapses. The printed part
   has no barb.
3. **Frame.** Three bars `oledBar` 3 mm wide and `oledBarHeight` 6 mm tall join the bosses: one
   along each side (west and east, north boss to south boss) and one along the south row. No
   bar along the north row, where the socket stands. The bars stop 2.0 mm under the module at
   the 8.0 seat (3 mm at the first 9.0), so the module's back-side parts clear them, and the
   module touches the adapter at the four boss tops and the posts only. The owner added
   (2026-10-28) a wall along the south row between the south bosses, outside the south bar at
   y -60.75 to -58.25, rising to the seat with its top inner edge chamfered 2.5 mm, and 1 mm
   chamfers where the six bar ends meet the bosses; both are feature `Chamfer1`.
4. **Holding on the board.** Four split pins under the bosses (owner 2026-10-27, pegs over
   screws; owner 2026-10-28, they must retain; owner 2026-10-30, final values by print).
   Each pin is `oledPegCore` Ø3.2, the hole's own diameter, `oledPegShank` 2.5 long, with the
   slot `oledPegSlot` 0.5 east to west through the whole pin and `oledSlotRise` 1.0 up into
   the boss, halves 1.35 thick. The swell makes it a press and the slot gives the halves room
   to close; the adapter presses on and stays. The pin shows 0.9 under a 1.6 board. Printed
   only, nothing bought. A barb feature `barbs` (Ø3.4 tapering over 1.0 to Ø2.4) remains in
   the timeline and fails for the same reason as the post barbs; the printed part has no barb.
   Crush ribs on pegs and posts were the first design and printed solid (2.7); plain Ø2.8 pegs
   were the second and held nothing; tapered barbs on Ø2.8 and Ø2.5 shanks were the third,
   superseded by the owner's values before a print.
5. **Model truth.** The module occurrence in the Fusion document moves `oledOffset` toward the
   rear edge so its holes sit on the posts, and the later lid work sees the glass where it is.
   As built the move is a captured position, timeline item `module offset`, +1.0 mm in y and
   -0.585 in z, which put the module's PCB on the seats when they stood at 8.5; the Fusion API
   cannot drive an occurrence from a parameter, so a change to `oledOffset` or `oledSeat`
   moves the adapter and the module is moved by hand to match. With the seat at 8.0 the module
   model floats 0.5 above the seats and its pin tips touch the board top, which is where the
   drawing's 8.5 mm pins would stop it. The module is left there until the pin length is known.
   The moved model's header pins stand 1.0 mm north of the socket's pin row; on the real module
   the pins stay in the socket and the PCB,
   holes and glass sit north, which is what the drawing gets wrong (2.4).
6. **Parameters**, in mm, the owner's values of 2026-10-30. `oledOffset` 1.0, `oledSeat` 8.0,
   `oledBoss` 8, `oledBar` 3, `oledBarHeight` 6; posts `oledPostCore` 3.0, `oledPostShank` 2.5;
   pegs `oledPegCore` 3.2, `oledPegShank` 2.5; slots `oledPegSlot` 0.5, `oledSlotRise` 1.0.
   The failed barb features keep `oledPostBarb` 3.2, `oledPostTaper` 0.65, `oledPostTip` 2.55,
   `oledBarb` 3.4, `oledBarbTaper` 1.0, `oledBarbTip` 2.4; `oledPostHeight` (3.15) and
   `oledPegHeight` (3.5) are shank plus taper and size only the slot cuts.

## 4. Build steps in Fusion

Built 2026-10-27 with ribs, reworked 2026-10-28 to plain chamfered columns, then to split snap
pegs with a banded barb, then to eight split snap pegs with tapered barbs; the owner then set
the final values and printed (2026-10-30). As saved: component `oled-adapter`, one body
`oled adapter`, 96 faces, 3326.93 mm³, z -0.984 to 12.016 in the board frame; `adapter sketch`
fully constrained; features in timeline order `bars`, `bosses`, `posts`, `pegs`, `Chamfer1`
(owner), `barbs` (fails), `peg slots`, `post barbs` (fails), `post slots`; timeline 204, the
two barb features in error; the 19 parameters of 3.6.

1. Component `oled-adapter` in `cubevox-enclosure`, beside `PCB:1`.
2. One sketch on the board's top face, `adapter sketch`: the four board holes projected; the
   four boss circles concentric with them; the four post circles `oledOffset` toward the rear
   edge from them, with their four barb circles `oledPostBarb`; the three bar rectangles
   between the bosses; the four peg circles and four barb circles `oledBarb` concentric with
   the holes; and eight slot rectangles `oledPegSlot` by 4 mm centred on the pegs and posts,
   long side east to west. Rectangles and circles only, every one dimensioned to a projected
   hole or a parameter, fully constrained.
3. Extrudes: `bars` to `oledBarHeight`; `bosses` to `oledSeat`; `posts` from the seat by
   `oledPostShank`; `post barbs` from the post shank end upward `oledPostTaper`, tapered so
   the tip is `oledPostTip`; `pegs` downward `oledPegShank` from the board's top face; `barbs`
   from the shank end downward `oledBarbTaper`, tapered to `oledBarbTip`. Last, `peg slots`
   and `post slots`, cuts of the eight rectangles from each tip to `oledSlotRise` inside the
   boss, the adapter body the only participant. No chamfers but the owner's `Chamfer1`.
4. The module occurrence moves `oledOffset` toward the rear edge (3.5).
5. Named sketch, features and parameters; no save; no KiCad; no STEP or STL.

## 5. Verification

1. **Model.** One body. Pins drawn at the hole diameters, so the model shows a touching fit;
   the print's swell makes the press. Slots 0.5 wide, roofs 1.0 inside the bosses. Seat tops
   at `oledSeat` above the board. Nearest adapter face to the socket at least 1.0 mm. Post
   tips under the glass top.

   Read 2026-10-30 from the saved document, in mm:

   | Check | Target | Measured |
   |---|---|---|
   | bodies | 1 | 1, 96 faces |
   | peg pins | Ø3.2, 2.5 long | eight half-cylinders r 1.6, z -0.984 to 1.516 |
   | post pins | Ø3.0, 2.5 tall | eight half-cylinders r 1.5, z 9.516 to 12.016 |
   | slot roofs | 1.0 inside the bosses | four faces at z 2.516 (board top + 1.0) and four at 8.516 (seat - 1.0) |
   | tapers | none in the printed part | no conical faces but `Chamfer1`'s six; `barbs` and `post barbs` in error |
   | seat tops above the board top face | 8.000 | 8.000 |
   | module PCB underside to seat tops | 0 | 0.500 float (3.5; the module stays at the drawing's pin stop) |
   | nearest adapter face to the socket | ≥ 1.0 | 6.12 in plan to the socket outline and pads; the module's header passes between the two north bosses |
   | post tips against the glass top | below | seat + 2.5; 0.2 under the glass when seated |
   | first print of these values | fits and holds | owner 2026-10-30: "perfect" |

   The board body in the document is 1.516 thick, against the board's nominal 1.6. The socket
   J117 has no body in the document; its clearance is taken in plan from the footprint's socket
   outline and pads.
2. **First print.** The adapter seats on the board by hand. The module presses onto the posts
   with light thumb pressure and its pins enter the socket fully. A post that will not enter
   its hole means the offset is off; `oledOffset` moves by 0.1 toward the side the hole shows
   light, and the adapter is reprinted. A module that rocks on its seats means a boss printed
   tall; the seat is cleaned up or `oledSeat` drops 0.1.

## 6. Printing

PETG, seat face down (the frame upside down, posts on the bed), supports from the bed under the
frame. The frame carries columns on both faces, so whichever face is down the bosses and bars
stand 2.0 or 2.75 mm above the bed on the columns and need support there; the earlier claim
of a support-free print was wrong. The post tips meet the bed, where the first layer flares.
The 1.0 mm slot roofs inside the bosses are 0.5 mm bridges.

## 7. Open

1. Resolved 2026-10-27: pegs, printed only. Screws from below (the DrumSynth way, a Ø3.0 bore
   up each boss and an M3 x 8 from under the board) were the alternative and are not built.
2. `oledOffset` is the owner's estimate of about 1 mm; the first print sets it.
3. The module's back-side part heights are not in the drawing; 3 mm clearance under the module
   is assumed enough.
4. Resolved 2026-10-28: the ribbed pegs and posts of the first print entered no hole (2.7).
   The second print's plain Ø2.8 pegs entered every hole and held nothing. Tapered barbs
   followed (owner 2026-10-28, no band, all eight). The owner then set the pins to the hole
   diameters with a 0.5 slot and printed: fits and holds (2026-10-30). The two barb features
   stay in the timeline in error; deleting them, with their six parameters, would leave the
   printed geometry unchanged.

## Files

Component `oled-adapter` in the Fusion document `cubevox-enclosure`. The module model
`hardware/tools/model_oled_sh1106.py` and the footprint
`hardware/cubevox.pretty/OLED-1.3-SH1106-I2C-4P.kicad_mod` carry the drawing figures. Reference
construction: `OLED adapter` in `DrumSynth enclosure - Faceplate rework`, read only.
