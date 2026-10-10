# Pre-fab final re-run of fab, plane, BOM and netlist checks, 2026-10-10

Board `hardware/cubevox.kicad_pcb` at d267881. Read-only. Zones refilled in memory only, board never saved. `git status` is clean.
Scripts and outputs are in `/tmp` (`dfm_audit*.py`, `dfm_p3` to `dfm_p6`, `prefab_plane_*.py`, `final_scripts/`, `final/`, `final-bom/`). KiCad 10.0.5.
"Then" is the value in `2026-10-10-prefab-dfm.md` or `-plane.md`. Limits are the brief's 4-layer table.

## 1. Fab checks (DFM)

| # | Item | Then | Now | Verdict |
|---|---|---|---|---|
| 1.1 | Gerber zip | v2 of 2026-10-09, 16 files | still the only zip, older than the board | FIX (carried) |
| 1.2 | Track width, min | 0.200 mm; 693 of 1168 F.Cu, 214 of 368 B.Cu at 0.2 | 0.200 mm; 693 of 1172 F.Cu, 217 of 368 B.Cu | OK |
| 1.3 | Vias | 270, all 0.6 / 0.3 mm, annular 0.150 mm | 272, all 0.6 / 0.3 mm, annular 0.150 mm | OK |
| 1.4 | Via inside SMD pad | 1 (D2.2) | 0 | OK |
| 1.5 | Track end off-centre on a via | 1 | 0 of 407 | OK |
| 1.6 | PTH pad drill, slots | 1.000 mm round, 0.600 mm slots | same | OK |
| 1.7 | Drill wall to In1/In2 fill | 0.301 mm | 0.301 mm; the new vias 0.401 mm | OK |
| 1.8 | Inner annular ring | pads 0.200 mm, vias 0.150 mm | same | OK |
| 1.9 | Hole to hole, different nets | 0.500 mm; 50 pairs under 1.0 mm | 0.500 mm, same pair (107.33, 56.80) and (106.53, 56.80); 49 pairs | OK |
| 1.10 | Hole to hole, same net | 0.635 mm | 0.635 mm | OK |
| 1.11 | NPTH to copper | 0.260 mm | 0.260 mm | OK |
| 1.12 | Board edge: pads, tracks, vias, fill, hole wall | 0.450, 0.650, 0.450, 0.500, 0.600 mm | same; outline 8 items, one loop | OK |
| 1.13 | Silk strokes under 0.15 mm | 327 (0.06 x147, 0.12 x166, 0.10 x14) | 329 (0.12 x168); R162 adds two 0.12 mm lines | FIX |
| 1.14 | Silk strokes at 0.15 mm | 513 in 79 footprints | 513 in 79 footprints | DECIDE |
| 1.15 | Silk text height | 1.000 mm, 56 texts | same | OK |
| 1.16 | Silk text thickness | 0.100 mm on RESET and BOOT | 0.150 mm on all 56 texts, RESET and BOOT included | OK |
| 1.17 | Silk to pad | min 0.004 mm, 0 touching, 168 footprints within 0.15 mm | same min, 0 touching, 169 footprints (R162) | DECIDE |
| 1.18 | Mask bridge | min 0.200 mm, 0 under 0.10 mm | same | OK |
| 1.19 | Zones | /5V 25.2 mm2; GND 25417 mm2 | /5V 25.2 mm2; GND 25415.1 mm2; 1 outline each, 0 isolated | OK |
| 1.20 | Fill to edge | 0.500 mm | 0.500 mm | OK |
| 1.21 | B.Cu parts | J101, J106, J108, SWPWR1 out of BOM and CPL | same, 0 rows | OK |
| 1.22 | SMT parts with BOM and CPL | 195 | 196 (R162) | OK |
| 1.23 | THT parts on F.Cu | 27 in BOM and CPL | 27 | OK |
| 1.24 | Ignored rules | 0 of 245 lack courtyard; J1 only type mismatch | 0 of 246; J1 only | OK |
| 1.25 | DRC | 9 `lib_footprint_mismatch`, 0 unconnected | same 9 refs, 0 unconnected, no clearance, hole, silk or dangling items | OK |
| 1.26 | DRC with `--schematic-parity` (new) | not run | 81 items; 6 footprints (R159, R160, R161, D110, K103, Q102) carry a bare library id without the `cubevox:` nickname | FIX |
| 1.27 | Parity field and attribute items | not run | 72 missing or differing fields (MPN 55, Section 10, Group 5, Note 1, Description 1) and 3 exclude-from-BOM flags on J101, J106, J108 | OK |

Fix for 1.1: regenerate the gerbers from d267881. Fix for 1.13: set every stroked silk width to 0.153 mm, as in the first audit. Fix for 1.26: set the six library ids to the `cubevox:` names. This has no fab effect.
Recommendation for 1.14 and 1.17: accept, for the reasons in the first audit.

### Requested confirmations

| Item | Result | Verdict |
|---|---|---|
| D2.2 via | gone; no via at (114.956, 40.285) and 0 vias inside any SMD pad; D2.2 GND track runs 0.94 mm to the via at (114.800, 41.590) | OK |
| RESET and BOOT silk | 0.150 mm thick, 1.000 mm high, F.Silkscreen | OK |
| R162 pads | 0402, 0.54 x 0.64 mm at (154.49, 125.22) and (155.51, 125.22); pad gap 0.480 mm | OK |
| R162 clearance | pads 0.230 mm and GND via 0.250 mm from the POT01 track at y 125.87 (JLC 0.09 mm, project 0.15 mm) | OK |
| R162 GND via (156.54, 125.22) | 0.30 mm drill, 0.60 mm pad, annular 0.150 mm; nearest hole 1.195 mm (GND via at (158.00, 124.90)); not in a pad | OK |
| LINE_N via (197.75, 80.60) | 0.30 / 0.60 mm; nearest hole 3.144 mm (5VA); copper clearance 0.350 mm to the 5VA B.Cu lane; plane wall 0.401 mm | OK |
| LINE_N via (190.70, 81.60) | 0.30 / 0.60 mm; nearest hole 3.825 mm (GND); copper clearance 0.721 mm; plane wall 0.401 mm | OK |
| LINE_N tracks | 12 segments, all 0.200 mm, 26.30 mm long; K103 pads 2, 3, 6, 7 carry IN_P, LINE_P, IN_N, LINE_N; pads 4 and 5 open | OK |
| Hole to hole under the 0.50 mm project rule | minimum 0.500 mm with zero margin, DRC reports no `hole_to_hole` | OK |

## 2. Ground plane and return path

| # | Figure | Then | Now | Verdict |
|---|---|---|---|---|
| 2.1 | Plane fill In1 and In2, coverage | 25417.0 mm2, 94.8 % | 25415.1 mm2, 94.8 %; one island each | OK |
| 2.2 | Erosion test | 2.0 mm leaves one piece of 19094 mm2; four slivers at 3.0 mm | 19059 mm2; the same four slivers (50.1, 17.1, 12.0, 10.4 mm2) | OK |
| 2.3 | Necks under 1 mm | one 0.30 mm web at (137.8, 146.5) | same web, no other piece at 0.5 mm erosion | OK |
| 2.4 | Track over an In1/In2 void | F.Cu 185.5 mm (4.39 %), B.Cu 204.0 mm (6.59 %) | 187.0 mm (4.41 %), 206.0 mm (6.65 %) | OK |
| 2.5 | Void under foreign nets | 2.0 mm; three slow signals | 2.0 mm; same three | OK |
| 2.6 | Longest gap, any net | 3.0 mm | 3.0 mm; no net over 5.0 mm | OK |
| 2.7 | LINE_N over own vias | 2.5 mm F.Cu, 2.0 mm B.Cu | 4.0 mm F.Cu, 4.0 mm B.Cu (the two new vias) | OK |
| 2.8 | Other sensitive nets | VREF 4.5 / 4.0 mm, PRE_OUT 2.5, INA_O2 2.0, I2S_BCLK 2.5 | identical | OK |
| 2.9 | Voids of 3 mm or 6 mm2 or more | 214 per plane, twins, largest 15 mm2 | 214, twins, 15 mm2 | OK |
| 2.10 | POT09 to POT16 slots | 7.7 x 1.13 mm and 7.9 x 1.12 mm | identical | DECIDE (unchanged) |
| 2.11 | GND vias | 152 plus 38 PTH pads; median 3.10 mm, max 24.00 mm | 152 plus 38; 3.10 mm, 24.00 mm | OK |
| 2.12 | Vias joining both planes | 152 of 152; 139 touch F.Cu GND | same | OK |
| 2.13 | Regions over 10 mm from a GND tie | largest 4796 mm2 at (189, 99) | identical top five | DECIDE (unchanged) |
| 2.14 | Signal vias, median to a GND tie | 118 vias, 3.94 mm | 120 vias, 4.05 mm | OK |
| 2.15 | Decaps | 47, all OK, worst 1.84 mm (C160) | 47, all OK, worst 1.84 mm | OK |
| 2.16 | Rail caps with no IC pin near | eight, C117 1.60 mm, C160 1.84 mm | identical | DECIDE (unchanged) |
| 2.17 | IC ground pins, long trace | U8.5 3.07 vs 2.34 mm; U6.9 2.37 vs 1.23 mm | identical | DECIDE (unchanged) |
| 2.18 | Buck loops | C113 1.26 mm2, C112 2.43 mm2, C20 1.48 mm2, C114 5.23 mm2; no analog track in the loop region | identical | OK |

### Requested figures

| Item | Result | Verdict |
|---|---|---|
| C127 loop | 7.34 mm then, 5.62 mm now (4.82 mm F.Cu to U105 pin 8, 0.80 mm GND trace to the via at (229.00, 78.50)); the script's 6 mm flag is not raised; the peer worst case is 5.24 mm (U6 pin 18) | DECIDE |
| New LINE_N vias to nearest GND tie | (197.75, 80.60) 6.8 mm; (190.70, 81.60) 4.1 mm; the old two stay at 5.0 and 4.7 mm | DECIDE |
| R162 pad 2 to its GND via | 1.03 mm centre to centre, straight 0.30 mm trace; a second GND via at (158.00, 124.90) is 2.5 mm away | OK |
| New void or neck from the two LINE_N vias and the R162 via | none; each LINE_N antipad is a lone 1.12 mm void with the nearest hole 3.1 mm and 3.8 mm away; the R162 via is GND and solid on both planes; void counts and erosion results are unchanged | OK |
| D2.2 return path | 0.94 mm of 0.30 mm trace, straight, to the via at (114.80, 41.59); it sits 0.10 mm off the pad edge | OK |

Recommendation for C127: accept at 5.62 mm. Recommendation for the LINE_N vias: add one GND via within 2 mm of (197.75, 80.60), as the first audit advised for LINE_N.

## 3. Assembly

Commands run from the repo root:

```
kicad-cli sch export netlist --format kicadsexpr -o /tmp/final-bom/cubevox.net hardware/cubevox.kicad_sch
kicad-cli pcb export pos --format csv --units mm --side front --exclude-dnp -o /tmp/final-bom/pos.csv hardware/cubevox.kicad_pcb
python3 hardware/tools/make_bom.py /tmp/final-bom/cubevox.net /tmp/final-bom/bom.csv --cpl /tmp/final-bom/pos.csv /tmp/final-bom/cpl.csv
```

| Check | Result | Verdict |
|---|---|---|
| Output | 73 BOM lines, 223 parts, 73 distinct LCSC, 223 CPL rows, exit 0, designators equal | OK |
| Rotations | 0 negative, range 0 to 270, all whole numbers | OK |
| R162 | BOM line `100k` with R158, C25741, `R_0402_1005Metric`; CPL (155.000, -125.220) Top rotation 0 | OK |
| K103, D110, Q102, R159 to R161 | present: C47190, C81598, C20526, C21190, C21190, C25803; D110 rotation 90, Q102 180, R159 270, R160 180, R161 0 | OK |
| J101, J106, J108, SWPWR1, TP1 to TP11 | absent from BOM and CPL | OK |
| LCSC against `hardware/PARTS.md` | all 77 LCSC on the board (73 BOM plus the four owner-soldered) appear in PARTS.md | OK |
| PARTS.md rows not on the board | row 19 (C470710, encoders) and row 29 (C192421, OPA1678), both marked removed or superseded, do not order | OK |
| C25741 stock (`jlc_get_part`) | 8,079,555 pieces, basic, 0402, 100 kohm 1 %; PARTS.md still lists 8,459,437 from 2026-10-08 | OK |
| Rotation offset table (earlier finding F3) | `make_bom.py` still has only the `% 360`; U107 writes 270 and U108 writes 0 from KiCad stock SOT-23 footprints with no JLC zero check | FIX (carried) |

Fix for the last row: add the footprint offset table for `SOT-23` and `SOT-23-5` and confirm U107 and U108 on the JLC placement preview.

## 4. Netlist and schematic

| Check | Result | Verdict |
|---|---|---|
| K103 netlist pins | 1 `/5V`, 2 `/IN_P`, 3 `/LINE_P`, 6 `/IN_N`, 7 `/LINE_N`, 8 `/PAD_COIL`; pins 4 and 5 have no net | OK |
| R162 netlist pins | 1 `/MUX_S0`, 2 `/GND` | OK |
| Board pads against netlist | 785 pads compared, 0 net mismatches, 0 netlist pins without a pad; K103 and R162 board nets equal the netlist | OK |
| `kicad-cli sch erc --severity-all` | 0 violations on all sheets; `wire_dangling` is not ignored in the project | OK |
| Wire ends within 30 units of K103 at (975.36, 571.5) | 12 ends; six land on K103 pins and six on net labels; none touches nothing; the six wired pins match the six netted pins | OK |

A whole-sheet scan of wire ends found 30 that touch nothing in my parser, all at x 83.82 on the h7core sheet pins, which my parser does not read. ERC reports none of them.

## Summary

Counts are verdict cells in the tables above.

| Verdict | Count | Items |
|---|---|---|
| FIX | 4 | 1.1 gerber zip stale; 1.13 silk strokes under 0.15 mm; 1.26 six bare library ids; make_bom.py rotation offsets for U107 and U108 |
| DECIDE | 8 | 1.14, 1.17, C127 loop, LINE_N via distance, and four unchanged plane items (2.10, 2.13, 2.16, 2.17) |
| OK | 61 | everything else, including all new K103, R162, C127, D2 and LINE_N geometry limits |

The new geometry (K103 re-lay, both LINE_N vias, R162 and its via, C127 on F.Cu, D2.2 without a via) meets every JLC limit and creates no new void, neck or DRC item.
