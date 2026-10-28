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
| 1 | Hold the module at the height the socket wants | module PCB underside `oledSeat` 9.0 mm above the board, so the pins enter the 5 mm socket 6.5 mm (hardware spec item 22) |
| 2 | Put the module's holes where the module's holes are | posts on the board's 30.4 x 28.5 grid slid `oledOffset` 1.0 mm toward the rear edge; `oledOffset` is a user parameter, changed after the first print |
| 3 | Hold the module without screws | posts with crush ribs in the module's Ø3.0 holes, as DrumSynth; the module presses on and pulls off by hand |
| 4 | Locate on the board without fighting the socket | four seats on the board's four Ø3.2 holes; nothing within 1 mm of the socket or the module's back-side parts |
| 5 | Be one printed PETG part, no supports | circles and rectangles in plan, every face rising from the bed or solid below; printed seat face down |
| 6 | Be adjustable by parameter | every figure in section 3.6 is a user parameter |

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
4. Observed (owner 2026-10-27): with the module plugged in, its holes sit about 1 mm north of
   the board's holes. The drawing's hole position is wrong by that amount; the grid itself is
   not in question.
5. The Fusion module model carries the drawing's holes, on the board's grid, so it does not
   show the offset.
6. The board underside under the module is bare. The nearest underside part is the combo at
   x 105 to 131.

## 3. Design

1. **Seats.** Four round bosses `oledBoss` Ø7 mm centred on the board's four holes, from the
   board top to `oledSeat` 9.0 mm. The module's PCB rests on their tops. Each boss also carries
   the post, which sits `oledOffset` off the boss centre; at 1.0 mm the post crest reaches
   2.7 mm from the boss centre, inside the Ø7.
2. **Posts.** On each boss top a post for the module hole: core `oledPostCore` Ø2.7, height
   `oledPostHeight` 2.0 mm above the seat, with four crush ribs `oledRibWidth` 0.9 mm wide to a
   crest `oledPostCrest` Ø3.4, straight, no taper. The core clears the Ø3.0 hole by 0.15 mm a
   side and the ribs interfere 0.2 mm a side; the ribs yield, the core never binds, so the
   offset may be wrong by 0.15 mm before anything fights. The post top stands 0.8 mm above
   the module PCB and 0.7 mm under the glass top, and the holes lie outside the glass in y.
3. **Frame.** Three bars `oledBar` 3 mm wide and `oledBarHeight` 6 mm tall join the bosses: one
   along each side (west and east, north boss to south boss) and one along the south row. No
   bar along the north row, where the socket stands. The bars stop 3 mm under the module, so
   the module's back-side parts clear them, and the module touches the adapter at the four
   boss tops and the posts only.
4. **Locating on the board.** Four pegs under the bosses (owner 2026-10-27, pegs over
   screws): core `oledPegCore` Ø3.0 with four crush ribs `oledRibWidth` wide to a crest
   `oledPegCrest` Ø3.5, `oledPegHeight` 4 mm, pressed into the board's Ø3.2 holes from above.
   The core clears the hole by 0.1 mm a side and the ribs interfere 0.15 mm a side. The pegs
   stand 4 mm under a 1.6 mm board, so 2.4 mm shows underneath, clear of the bare underside.
   Printed only, nothing bought. The seat stays at 9.0 and 7 mm of solid boss sits between the
   peg and the post.
5. **Model truth.** The module occurrence in the Fusion document moves `oledOffset` toward the
   rear edge so its holes sit on the posts, and the later lid work sees the glass where it is.
   As built the move is a captured position, timeline item `module offset`, +1.0 mm in y; the
   Fusion API cannot drive an occurrence from a parameter, so a change to `oledOffset` moves the
   posts and the module is moved by hand to match. The moved model's header pins stand 1.0 mm
   north of the socket's pin row; on the real module the pins stay in the socket and the PCB,
   holes and glass sit north, which is what the drawing gets wrong (2.4).
6. **Parameters**, in mm. `oledOffset` 1.0, `oledSeat` 9.0, `oledBoss` 7, `oledPostCore` 2.7,
   `oledPostCrest` 3.4, `oledPostHeight` 2.0, `oledRibWidth` 0.9, `oledBar` 3,
   `oledBarHeight` 6, `oledPegCore` 3.0, `oledPegCrest` 3.5, `oledPegHeight` 4.

## 4. Build steps in Fusion

Built 2026-10-27 as below: component `oled-adapter`, one body `oled adapter`, 154 faces,
2777.98 mm³, x 61.085 to 98.485, y -60.25 to -24.75, z -2.484 to 12.516 in the board frame;
`adapter sketch` fully constrained, 19 rectangles, 12 circles, 86 dimensions, 94 constraints, the
four board holes projected as construction circles; six features `bars`, `bosses`, `posts`,
`post ribs`, `pegs`, `peg ribs`; timeline 201, none unhealthy; the 12 parameters of 3.6. The
ribs are two crossing rectangles per post and per peg, each spanning the crest diameter.

1. Component `oled-adapter` in `cubevox-enclosure`, beside `PCB:1`.
2. One sketch on the board's top face, `adapter sketch`: the four board holes projected; the
   four boss circles concentric with them; the four post core circles `oledOffset` toward the
   rear edge from them; the rib rectangles on each post; the three bar rectangles between the
   bosses; the four peg core circles concentric with the holes; and the rib rectangles on each
   peg. Rectangles and circles only, every one dimensioned to a projected hole or a parameter,
   fully constrained.
3. Extrudes: `bars` to `oledBarHeight`; `bosses` to `oledSeat`; `posts` from the seat by
   `oledPostHeight`; `post ribs` the same; `pegs` downward `oledPegHeight` from the board's
   top face; `peg ribs` the same.
4. The module occurrence moves `oledOffset` toward the rear edge (3.5).
5. Named sketch, features and parameters; no save; no KiCad; no STEP or STL.

## 5. Verification

1. **Model.** One body. Interference with the board: the peg rib crests into the hole walls
   by 0.15 mm a side, and nothing else. Interference with the moved module: the post
   crests into its holes by 0.2 mm a side, and nothing else. Seat tops at 9.000 above the
   board. Post cores concentric with the moved module's holes. Bars 3.000 under the module.
   Nearest adapter face to the socket at least 1.0 mm. Post tops under the glass top.

   Measured 2026-10-27, agent build and a second read-only audit, in mm:

   | Check | Target | Measured |
   |---|---|---|
   | bodies | 1 | 1 |
   | interference with the board | peg ribs into the hole walls only | 1 pair, 3.741 mm³, z 0 to 1.516, crest r 1.75 in r 1.6 holes |
   | interference with the moved module | post ribs into its holes only | 1 pair, 3.850 mm³, z 10.601 to 11.801, crest r 1.7 in r 1.5 holes |
   | interference with the other 93 bodies on `PCB:1` | none | none; nearest is 14.2 away |
   | seat tops above the board top face | 9.000 | 9.000, four faces at z 10.516 |
   | post cores to the moved module's holes | concentric | 0.000 at all four |
   | bar tops under the module PCB underside | 3.000 | 3.085 (see below) |
   | nearest adapter face to the socket | ≥ 1.0 | 6.62 in plan to the socket outline and pads; 6.623 to the module's header body, which passes between the two north bosses |
   | post tops against the glass top | below | 0.785 below (post top z 12.516, glass top 13.301); 0.715 above the module PCB top |
   | pegs below the board underside | 2.4 | 2.484 (board body 1.516 thick) |

   The board body in the document is 1.516 thick, against the board's nominal 1.6, while the
   module model stands on the KiCad board top at z 1.601. In the model the module therefore
   floats 0.085 above the seats and the bars read 3.085 under it. The print is unaffected; the
   seat is 9.0 above the board's real top face. The socket J117 has no body in the document;
   its clearance is taken in plan from the footprint's socket outline and pads.
2. **First print.** The adapter seats on the board by hand. The module presses onto the posts
   with light thumb pressure and its pins enter the socket fully. A post that will not enter
   its hole means the offset is off; `oledOffset` moves by 0.1 toward the side the hole shows
   light, and the adapter is reprinted. A module that rocks on its seats means a boss printed
   tall; the seat is cleaned up or `oledSeat` drops 0.1.

## 6. Printing

PETG, seat face down (the frame upside down, posts on the bed), no supports. The posts and ribs
print first and the bosses and bars rise from them; the pegs print last, upward, from the boss
tops. Everything is a column or a bar; nothing overhangs.

## 7. Open

1. Resolved 2026-10-27: pegs, printed only. Screws from below (the DrumSynth way, a Ø3.0 bore
   up each boss and an M3 x 8 from under the board) were the alternative and are not built.
2. `oledOffset` is the owner's estimate of about 1 mm; the first print sets it.
3. The module's back-side part heights are not in the drawing; 3 mm clearance under the module
   is assumed enough.
4. Peg fit in a Ø3.2 drilled hole is untested; if the pegs will not enter, `oledPegCrest`
   drops 0.1 and the adapter is reprinted; if the adapter lifts off, it rises 0.1.

## Files

Component `oled-adapter` in the Fusion document `cubevox-enclosure`. The module model
`hardware/tools/model_oled_sh1106.py` and the footprint
`hardware/cubevox.pretty/OLED-1.3-SH1106-I2C-4P.kicad_mod` carry the drawing figures. Reference
construction: `OLED adapter` in `DrumSynth enclosure - Faceplate rework`, read only.
