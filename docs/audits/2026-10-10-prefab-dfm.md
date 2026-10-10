# Pre-fab DFM audit vs JLC capability, 2026-10-10

Board `hardware/cubevox.kicad_pcb` at c2635eb. Read-only. Nothing under `hardware/` was written or saved.

Scripts (all in `/tmp`, KiCad 10.0.5 python): `dfm_audit.py` (items 1 to 10), `dfm_audit2.py` (via and library checks), `dfm_p3.py` (zone to edge), `dfm_p4.py` (silk detail), `dfm_p5.py` (courtyard, type). DRC: `kicad-cli pcb drc --severity-all --format json` to `/tmp/dfm_drc.json` and again with `--refill-zones` to `/tmp/dfm_drc_refill.json`. Both give 9 violations, 0 unconnected.

Limits are the brief's 4-layer table (rewritten after the first pass of this audit, which used the old 2-layer table). Where the JLC capabilities page (fetched 2026-10-10) differs, the row says so.

## 0. Stackup and gerber job

| Item | Brief | Board | Verdict |
|---|---|---|---|
| Copper layers | 4 layers, JLC04161H-7628 | F.Cu, In1.Cu (GND zone), In2.Cu (GND zone), B.Cu; 1.6 mm; outer 35 um, inner 15.2 um; HASL lead-free | OK |
| Gerber job includes inner layers | In1_Cu and In2_Cu present | `hardware/production/cubevox-gerbers-v2.zip` has 16 files incl. `cubevox-In1_Cu.g1`, `cubevox-In2_Cu.g2`; its `.gbrjob` says 4 copper layers | OK |

1. The v2 zip is dated 2026-10-09. The board changed after it in commits 560d966 and c2635eb (2026-10-10). The zip does not match the board. Regenerate before upload; the brief already says the v3 files are pending.
2. The brief says F.Cu and B.Cu also carry GND fill zones. The board has no GND zone on either outer layer. It has only the 25.2 mm2 /5V zone on F.Cu. This is not a fab defect, but ground return on the outer layers is by tracks and vias to the planes only.

## 1. Track width

| JLC limit | Board minimum | Location | Verdict |
|---|---|---|---|
| 0.09 mm | F.Cu 0.200 mm (693 of 1168 segments), B.Cu 0.200 mm (214 of 368) | F.Cu (65.750, 101.040) to (65.750, 98.400) /DAC_AC; B.Cu (108.500, 113.700) to (112.500, 109.700) /TOGGLE2 | OK |

1. Segments under 0.2 mm: none on either layer. Other widths: 0.3, 0.4, 0.5 mm.
2. The DRC `min_clearance` is 0.15 mm and the DRC is clean, so copper clearance is at least 0.15 mm against the 0.09 mm limit. Inner layers carry only the two GND planes, no tracks.

## 2. Vias

| Item | JLC limit | Board minimum | Location | Verdict |
|---|---|---|---|---|
| Drill | 0.20 mm | 0.300 mm | all 270 vias | OK |
| Diameter | 0.40 mm | 0.600 mm | all 270 vias | OK |
| Annular ring | 0.10 mm | 0.150 mm (outer and inner) | all 270 vias | OK |
| Via inside an SMD pad | no open via in a pad | 1 via inside pad D2.2 (USBLC6, GND) | via (114.956, 40.285) /GND, pad D2.2 (114.800, 40.650), pad 0.532 x 1.072 mm | FIX |

1. One via size only: 270 vias at 0.6 mm over 0.3 mm. Tenting is on, front and back.
2. The D2 via sits inside pad 2, 0.397 mm from the pad centre, with its barrel half outside the pad. The tented but unplugged 0.3 mm hole can wick solder off the GND pad of a SOT-23-6 and JLC's DFM flags it. Fix: move the via off the pad onto a short stub, or order plugged vias. Moving it is the better one because it costs nothing at the fab.

## 3. Holes

| Item | JLC limit | Board minimum | Location | Verdict |
|---|---|---|---|---|
| Round PTH pad drill | 0.30 mm | 1.000 mm | 51 round holes; smallest slot width 0.6 mm (next row) | OK |
| Drill wall to In1/In2 GND fill, other net | 0.20 mm | 0.301 mm | 241 non-GND holes and NPTH checked per plane; minimum at the NPTH pegs of J101, J117 and the M3 mounts, e.g. H101 (61.000, 35.000) | OK |
| Inner annular ring | 0.10 mm (page: 0.15 mm absolute minimum) | PTH pads 0.200 mm (J1.1 to J1.4 on In1.Cu and In2.Cu); vias 0.150 mm | 290 PTH pad inner rings, 270 vias | OK |
| Plated slots | width 0.65 mm in the brief; 0.50 mm (2-layer) and 0.35 mm (multilayer) on the capabilities page | 0.600 mm wide | J1.1, J1.2 0.6 x 1.7 mm (119.320, 35.550), (110.680, 35.550); J1.3, J1.4 0.6 x 1.4 mm (119.320, 31.400), (110.680, 31.400) | OK |
| Hole to hole, different nets | 0.50 mm | 0.500 mm | vias (107.330, 56.800) /GND and (106.530, 56.800) /5V | OK |
| NPTH to copper | 0.254 mm | 0.260 mm | NPTH J1 peg (117.890, 35.050) to pad J1.A1B12 (118.200, 36.165) | OK |

1. Slots by size, all plated unless noted: pot lugs 1.8 x 2.25 and 1.8 x 2.3 mm (36 holes); toggle pins 1.0 x 2.0 mm (27); encoder shell 1.7 x 2.8 mm (2); DC jack 1.0 x 3.5, 1.0 x 3.0, 3.0 x 1.0 mm (3); output jack 0.9 x 1.8 mm (6); J1 shield 0.6 x 1.7 and 0.6 x 1.4 mm (4). No NPTH slots.
2. The 0.65 mm slot figure in the brief is stricter than the page. The 0.6 mm J1 slots pass the page's 0.5 mm and 0.35 mm. Length is at least 2x width on every J1 slot (2.3x and 2.8x). The pot lug ovals have a 1.25 length to width ratio, which JLC routes as slots. The page's 2x ratio line may apply. Confirm in the JLC DFM viewer after upload.
3. Hole to hole: no different-net pair under 0.50 mm. The minimum has zero margin against the brief figure and 0.05 mm against the page's 0.45 mm for pad holes. 50 hole pairs are under 1.0 mm edge to edge. Same-net minimum is 0.635 mm.
4. NPTH to copper: 16 NPTH (4 M3 mounts 3.2 mm, 4 OLED holes 3.2 mm, 2 J1 pegs 0.65 mm, 3 J3 pegs 0.991 mm, 3 J101 pegs 1.8 mm). Margin to the limit is 0.006 mm, set by J1's vendor land pattern. `hardware/cubevox.kicad_dru` relaxes hole clearance to 0.255 mm inside J1 only, and that matches.
5. Smallest PTH pad annular ring: 0.200 mm (J1 shield slots).

## 4. Board edge

| Item | JLC limit | Board minimum | Location | Verdict |
|---|---|---|---|---|
| Pads | 0.30 mm | 0.450 mm | J1.3 (119.320, 31.400) /GND | OK |
| Tracks | 0.30 mm | 0.650 mm | B.Cu and F.Cu BOOT0 track ending at (56.800, 94.350) | OK |
| Vias | 0.30 mm | 0.450 mm | via (56.800, 94.350) /BOOT0 | OK |
| Zone fill, inner and outer | 0.30 mm | 0.500 mm | In1.Cu and In2.Cu GND fill at (240.500, 33.000); 5V F.Cu fill 13.600 mm | OK |
| Inner-layer pad rings | 0.30 mm | 0.450 mm | J1.3 (119.320, 31.400), same position as the outer pad | OK |
| Outline | one closed loop | 8 items (4 lines, 4 arcs), every endpoint shared by exactly 2 items, one connected loop, one outline polygon, 0 holes; 185.100 x 145.100 mm, 3 mm corner radius | Edge.Cuts | OK |

1. Plane edge distance was computed against the 56 to 241 mm by 30 to 175 mm rounded rectangle.
2. Hole walls to the edge: 0.600 mm minimum (the BOOT0 via).

## 5. Silkscreen

| Item | JLC limit | Board minimum | Location | Verdict |
|---|---|---|---|---|
| Stroke width under 0.15 mm | 0.153 mm (page 0.15 mm) | 0.06 mm | 327 stroked items; see detail | FIX |
| Stroke width exactly 0.15 mm | 0.153 mm (page 0.15 mm) | 0.150 mm | 513 line and arc items in 79 footprints | DECIDE |
| Text height | 1.0 mm | 1.000 mm, all 56 texts | F.SilkS 52, B.SilkS 4 | OK |
| Text thickness | 0.153 mm | 0.100 mm | board text RESET (58.200, 85.000) and BOOT (58.200, 100.800); the other 54 texts are 0.15 mm | FIX |
| Silk to pad | at least 0.15 mm, none on pads | 0.004 mm; 0 items touch a pad | 168 footprints have silk within 0.15 mm of a pad; detail below | DECIDE |
| DRC | baseline 9 lib_footprint_mismatch | 9, nothing else | see list | OK |

1. Stroke items under 0.15 mm: 0.06 mm x 147 (pin 1 circles, 144 footprints on F.SilkS and 3 on B.SilkS), 0.12 mm x 166 (155 lines, 11 circles), 0.10 mm x 14. Fix: set every stroked silk width to 0.153 mm in the library copies and the board, and check the pin 1 dots still sit clear of their pads.
2. The 0.15 mm items are 0.003 mm under the brief's 0.153 mm and equal to the capabilities page's 0.15 mm. Recommendation: accept them. Raise them only if the fix in item 1 is scripted anyway, since it is the same one-line change.
3. Filled polygons with 0 width (35 items, 14 footprints) are not strokes and are not counted.
4. Silk to pad, by smallest gap per footprint: under 0.01 mm 12 footprints, under 0.05 mm 81, under 0.10 mm 18, under 0.15 mm 57. Pin 1 circles sit 0.004 to 0.028 mm off the pad. The mask opening equals the pad, so JLC's silk clipping removes ink over openings and no item overlaps a pad. Recommendation: accept, because no silk lands on copper and the cost is a possible broken mark edge. Pot RV1xx (0.103 mm), toggles (0.059 mm), D107 and D108 (0.040 mm) are the largest non-passive cases if the owner wants them cleaned.
5. DRC `silk_over_copper`, `silk_overlap`, `silk_edge_clearance`, `text_height` and `text_thickness` all report 0.
6. The 9 `lib_footprint_mismatch` footprints, compared with the `cubevox` library copy (pad positions, sizes, shapes, layers, drills, mask and paste margins all identical in every case):

| Ref | Footprint | Difference | Matters for fab |
|---|---|---|---|
| C121, C125, C139, C141, C160 | CAP-SMD_BD4.0-L4.3-W4.3-FD | 4 outline lines moved from F.Silkscreen (library) to F.Fab (board) | No, silk removed near pads |
| C161 | C0603 | 11 outline items moved from F.Silkscreen to F.Fab | No |
| R147 | R0603 | 7 outline items moved from F.Silkscreen to F.Fab | No |
| J117 | OLED-1.3-SH1106-I2C-4P | the library has one extra F.Fab text | No |
| U106 | SOIC-8_L4.9-W3.9-P1.27-LS6.0-BL | 2 silk lines moved to F.Fab; library has an extra F.Fab text | No |

## 6. Solder mask

| Item | JLC limit | Board | Location | Verdict |
|---|---|---|---|---|
| Expansion settings | none | global expansion 0.000 mm, mask min width 0.000 mm, mask to copper 0.000 mm, vias tented front and back; local 0.5 mm on 3 fiducials FID1 to FID3 only | board setup | OK |
| Mask bridge | 0.10 mm | smallest copper gap between pads 0.200 mm; 0 pairs under 0.10 mm | J1 (USB-C, 0.5 mm pitch) and U1 (0.5 mm pitch), e.g. J1.A1B12 / J1.A4B9 (118.200, 117.400; 36.165) | OK |

1. With expansion at 0 mm the bridge equals the copper gap, 0.200 mm. If JLC applies its own 0.05 mm per side expansion (not verified here), the bridge on the 0.5 mm pitch parts is 0.10 mm, which is at the limit and still passes.
2. No other pad pair within 0.5 mm of copper has a gap under 0.2 mm.

## 7. Zones

| Zone | Layer | Net | Min width | Thermal gap / spoke | Pad connection | Islands | Fill | Verdict |
|---|---|---|---|---|---|---|---|---|
| 5V buck output | F.Cu | /5V | 0.200 mm | 0.500 / 0.500 mm | thermal | remove always, min island 10 mm2 | filled; fresh fill 1 outline, 25.2 mm2, 0 isolated | OK |
| GND plane | In1.Cu | /GND | 0.200 mm | 0.500 / 0.500 mm (unused) | solid | remove always, min island 10 mm2 | filled; fresh fill 1 outline, 25417 mm2, 0 isolated | OK |
| GND plane | In2.Cu | /GND | 0.200 mm | 0.500 / 0.500 mm (unused) | solid | remove always, min island 10 mm2 | filled; same as In1 | OK |

1. Zone clearance is 0.25 mm on all three. The refill was in memory only; the file is unchanged (`git status` shows only the brief as untracked).
2. 12 rule areas also exist (fiducial windows FID1 to FID3 on F.Cu, M3 seats H101 to H104 on F.Cu and B.Cu). They are keepouts with no fill.
3. DRC after refill reports no `isolated_copper` and no `copper_sliver`.

## 8. Parts on B.Cu

| Ref | Type | in_bom | dnp | exclude from pos | In production BOM / CPL | Verdict |
|---|---|---|---|---|---|---|
| J101 (NCJ6FA-H combo input) | THT | no | no | yes | 0 rows / 0 rows | OK |
| J106 (PJ-002A barrel) | THT | no | no | yes | 0 / 0 | OK |
| J108 (PJ-611E output) | THT | no | no | yes | 0 / 0 | OK |
| SWPWR1 (SS-12D11-G050) | THT | no | no | yes | 0 / 0 | OK |

1. Only the 4 owner-soldered parts are on B.Cu. All 195 SMT parts with BOM and position output are on F.Cu.
2. Other F.Cu parts excluded from BOM and position files: TP1 to TP11, H101 to H104, J3, FID1 to FID3 (19 footprints in all).
3. Evidence: `grep` of `cubevox-bom-v2.csv` and `cubevox-cpl-v2.csv` for the four refs returns 0 rows.

## 9. Through-hole parts on F.Cu

| Refs | Count | in_bom | dnp | exclude from pos | In production BOM / CPL | Verdict |
|---|---|---|---|---|---|---|
| RV101 to RV116, RVGAIN1 (pots) | 17 | yes | no | no | yes / yes | OK |
| SW101 to SW108, SWBYPASS1 (toggles) | 9 | yes | no | no | yes / yes | OK |
| ENC101 (encoder) | 1 | yes | no | no | yes / yes | OK |

1. JLC fits these 27 THT parts, so they belong in the BOM and CPL. All 27 are in `cubevox-bom-v2.csv` (pots C361173 and C470307, toggles C1788487, encoder C470754) and in `cubevox-cpl-v2.csv`, none flagged DNP or excluded.
2. J117 (OLED socket) is an SMD-attribute part in the same files. J1 (USB-C) carries PTH shield slots under an SMD attribute, which is the one `footprint_type_mismatch` case in item 10.
3. The first pass of this audit judged these as owner-soldered from the old brief and marked them FIX. That was wrong under the corrected brief.
4. The CPL origin and rotation of THT parts follow the same KiCad position export as the SMT parts. The rotation offsets for THT footprints (pot, toggle and encoder bodies) were not checked here.

## 10. Rule severities set to ignore

`hardware/cubevox.kicad_pro` ignores 4 rules.

| Rule | What it could hide | Check | Verdict |
|---|---|---|---|
| `track_not_centered_on_via` | a track end that touches a via off-centre | 404 track ends sit within 0.005 mm of a via centre; 1 end overlaps via copper off-centre: track (114.800, 40.650) to (114.800, 41.590) /GND at via (114.956, 40.285), 0.397 mm off | OK, the one hit is the item 2 FIX |
| `missing_courtyard` | parts with no courtyard | 0 of 245 footprints lack a courtyard | OK |
| `footprint_type_mismatch` | SMD/THT attribute disagreeing with pad types | 1 footprint: J1 is SMD with PTH shield slots, which JLC places as SMT | OK |
| `footprint_filters_mismatch`, `tuning_profile_track_geometries` | library filter and tuning profile bookkeeping | no fab effect | OK |

Warnings in the project that are not ignored (`hole_to_hole`, `silk_over_copper`, `isolated_copper`, `track_dangling`, `via_dangling`) all report 0.

## Summary

Counts are verdict cells in the tables above.

| Verdict | Cells | Findings |
|---|---|---|
| FIX | 3 | D2 via in pad (item 2); silk strokes under 0.15 mm (item 5); silk text thickness 0.10 mm (item 5) |
| DECIDE | 2 | silk strokes at exactly 0.15 mm and silk within 0.15 mm of pads, both with a recommendation to accept (item 5) |
| OK | 36 | everything else, including the 4-layer stackup, the gerber job layers and the 27 THT parts |

The v2 gerber zip predates two board commits and needs regeneration before upload (item 0).
