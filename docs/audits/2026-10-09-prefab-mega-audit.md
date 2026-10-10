# Pre-fab routing audit, 2026-10-09

Board on disk: `hardware/cubevox.kicad_pcb` saved 19:07 (PAD_EN laid by the owner, uncommitted).
Bar: canon 44, 45, 48, 132 to 140 (ClaudeRouter `canon.md`), owner rules (straight centred
pad entry, no sub-0.5 mm jogs, no unnecessary vias), professional craft.

## 1. Method

1. `hardware/tools/gates.sh` (DRC + parity, strict via audit, stray traces, pad entry, corners, angles).
2. `hardware/tools/mega_audit.py` (new, read-only): loops as a node graph, micro segments, stair-step
   jogs with a clearance-tested replacement (canon 136's own test), acute joints, mergeable collinear
   runs, near-miss joins, off-centre pad and via ends, long parallel runs typed analog/digital/power,
   via spacing, plane antipad slots, decoupling topology, routed GND, off-pad T junctions, bends at a
   pad or via edge, width steps, copper under the crystal.
3. Cycle paths traced per power net (`/tmp/cyc.py`); candidate re-routes clearance-tested at 0.2 mm
   against KiCad's own pad, track and via shapes (`/tmp/pc.py`).

## 2. Gate state on disk

| gate | result |
|---|---|
| DRC + parity | 9 lib mismatches (baseline), 0 unconnected |
| via_audit_strict | 0 true orphans, 270 vias (264 + PAD_EN 2 + LINE_N 2 + 3V3 hop 2) |
| route_audit | FAIL: 1 stray, G2_INV 0.1 mm segment into C132.2 |
| pad_entry | FAIL: 2, the same G2_INV segment (45 degree entry) |
| corner_check | 6 corners, all registered |
| angles | 0 off-angle, 0 long diagonals |

## 3. Findings by class

| class | found | defects | kept with reason |
|---|---|---|---|
| copper loops (133) | 1 real | 3V3 trunk to R128, R127, x 182, y 82, R129, trunk (22 x 45 mm, present in c4788e2) | 0 |
| off-pad T junctions (44/45) | 4 | 3V3 (159.97,58.90) acute 45; LINE_P (214.10,73.60) acute 45 | 3V3 bar/U8 E0147; USB_DM E0148 (registered) |
| stair-step jogs (136) | 12 | 8 where one straight + one diagonal fits: TOGGLE2 B.Cu, I2S_DATA_SRC, 3V3 at C57, VREF at R111, VBUS at J1, VREF_BUF across U108, 9V_PLUG at J106 | 3V3 B.Cu (90.65), TOGGLE1 B.Cu, SWCLK, SWO (E0111/E0112): no clear alternative; TOGGLE1 F.Cu dog-leg forced by SW101.1, chamfer legs to 0.3 |
| sub-0.5 lane shifts | 5 | G2_INV 0.07+0.14 (owner's), INA_O2 0.10 (C129 vs R110), PRE_OUT 0.37 single diagonal is fine | ENC_MENU_B 0.15 (MUX_A lane at y 80.5 pins the via), VNEG 0.13 (C35 position pinned by CAPM, lead 2026-10-08) |
| bends at a pad or via edge (132) | 20 | all 20: diagonals touching C4.1 C57.1 U1.75 R135.1 C47.1 J3.1 FB1.1 U1.13 U104.3 J1.A4B9 U108.1 U108.4 R27.1; vias at TOGGLE1/2 (0.25 chamfers at the via rim), ADC_SCKI via, GND via (122.90,103.80); bends inside Q101.3, K103.5, C132.2 | 0 |
| off-centre pad / via ends (132) | 26 | buck cluster U103/C116/L101/C113/C112 (10 ends), D105/D106 (2), U1.73 VCAP2 0.75, Q101.3 0.5, VBUS via 0.3 (2), GND vias at U8 0.275 (4), K103.5 0.01, C132.2 0.1 (2), C116.2 pass-through 0.1 | 0 |
| mergeable collinear splits | 3 | 3V3 (82.60,125.60) (85.00,125.60) (83.06,102.67) | 0 |
| near-miss joins | 0 | | |
| acute joints (45) | 2 | the two T junctions above | |
| parallel runs | 40 | 0 | PAD_EN beside VA_SENSE B.Cu 0.40 mm x 39 mm: DC level beside a slow sense line. MUX_S2 beside POT_MUX_OUT F.Cu 0.60 mm x 28 mm: select edges couple into the ADC line, but the mux settles before the sample (2 us); accepted, noted. VBUS_SENSE/USB_DM, XSMT/LDOO, INA_O1/O2, IN_P/A1_FB, POT buses, TOGGLE bus, USB pair at 0.20: audio-band or DC pairs, harmless |
| via pairs under 0.8 mm | 0 | | 2 pairs at exactly 0.80 (plane stitches beside VIN_9V and 5V vias) |
| plane antipad slots | 2 | 0 | POT09-16 via rows, 8 vias each, 6.6 mm diagonal slot in In1/In2 at (145.6-150.4, 125.9-130.4) and (153.1-158.0); nothing but POT lines crosses them |
| decoupling (63/68) | 10 plane-fed | 0 | C18 C112 C21 C22 C29 C30 C127 C134 C160 C117 are bulk or filter caps, not pin decouplers; every IC decoupler reaches its pin on F.Cu with its own GND via |
| routed GND (138) | 1 | TP3 to J106.1, 4.05 mm F.Cu trace | 0 |
| width steps (48) | 5 nets | VREF 3 x 0.20 at R146 (rest 0.30) | 3V3 0.40 L1 to C22/C21 legs; 5V 0.30 K103 coil branch (28 mA, Power class default); VBUS 0.20 sense loop (D0337); VCAP1 0.30 neck at the U1 pad |
| copper under Y1 | 0 | | |
| stray vias | 0 | | 13 GND vias touch no outer copper: 9 returns beside I2S/ADC/PRE_OUT layer changes, 4 sole plane stitches (RUNLOG via trim) |
| micro segments < 0.4 mm | 17 | the jog and edge-bend items above account for them | chamfers of 0.42 (0.3 legs) stay |

## 4. Fix list (nothing applied yet)

1. 3V3 loop + acute T: delete (159.97,58.90)-(160.72,59.65) and (160.72,59.65)-(169.94,59.65);
   R128 and R127 are then fed from R129 round x 182 (the loop's other half). Merge the trunk into one
   segment (159.97,49.43)-(159.97,78.87).
2. LINE_P acute T: delete (213.40,70.80)-(214.10,71.50) and (214.10,71.50)-(214.10,73.60); extend the
   lane y 70.80 east to (218.10,70.80), 45 to (218.50,71.20), south into C121.1 centre. The chain is
   then K103.3, lane, C121.1, west along y 72.16, 45 to (214.10,73.60), south to U104 side. Tested clear.
3. G2_INV + PRE_OUT at C132: C132 moves 0.07 west to x 231.00 (pads 230.30 / 231.70). PRE_OUT: pot pad
   (230.00,68.50) south to 70.00, 45 to (230.36,70.36), south on x 230.36 through C132.1 into U105.7
   centre. G2_INV: pot pad (232.50,68.50) 45 to (231.63,69.37), south on x 231.63 through C132.2 into
   U105.6 centre. One line each, pass-through offsets 0.06 / 0.07. Tested clear.
4. 3V3 at C57 (jog + two edge bends): C13 moves 0.30 north to (90.00,73.20) (pads 73.975 / 72.425),
   its GND via to (90.00,71.50). 3V3: U1.75 centre north to (90.00,75.45), 45 to (89.03,74.48), west
   into C57.1 centre. VCAP2: U1.73 centre north on x 91.00 to 74.225, 45 to (90.75,73.975), west into
   C13.1 centre (also fixes the 0.75 off-centre end). Tested clear with C13 moved.
5. VREF_BUF across U108: pad 1 centre east to (159.99,88.55), one 45 to pad 4 centre. Tested clear.
6. VBUS at J1: pad A4B9 south to (117.40,36.965), one 45 to (120.11,39.675). Tested clear.
7. VBUS at C18 (via rim bend): C18 moves 0.20 north (pad 1 at y 39.80); via to (111.11,39.80); B.Cu
   (112.61,39.50)-(112.31,39.80)-(111.11,39.80); F.Cu via east into C18.1; via south 0.40, 45 of 0.30
   legs, west on the y 40.50 lane (lane pinned by R5.1 above and R5.2 below). To test.
8. VREF at R111: (231.04,94.20) 45 to (232.15,93.09), north to 91.55, chamfer to (232.45,91.25), east.
   Tested clear.
9. TOGGLE2 B.Cu: (88.50,114.10) east to (91.00,114.10), one 45 to (94.50,110.60). Tested clear.
10. I2S_DATA_SRC: (87.00,93.60) 45 to (88.15,92.45), north to 90.45. Tested clear.
11. 9V_PLUG: (215.30,42.20) north to 39.20, 45 to (217.10,37.40). Tested clear.
12. TOGGLE1 F.Cu: chamfer legs 0.2 to 0.3, east run at y 116.30 (SW101.1 keeps 0.35).
13. TOGGLE1/TOGGLE2 vias at y 107.48: vias to x 110.60 / 112.40, straight exit 0.40, chamfer legs 0.30
    into the x 111.30 / 111.70 verticals; B.Cu feeds re-aimed. To test.
14. ADC_SCKI: via to (128.25,101.20) on the U8.6 lane; F.Cu straight west into U8.6; B.Cu
    (127.85,102.30)-(128.25,101.90)-(128.25,101.20). Removes two bends. To test.
15. GND vias at U8 (123.00 / 124.40, 101.30): vias 0.35 north to y 100.95, 45 legs start 0.5 later on
    the y 101.85 lane, straight 0.40 into each via. Tested clear at the new spots.
16. GND via (122.90,103.80): same recipe, straight exit 0.40.
17. MUTE_COIL: Q101.3 centre north 0.50, 45 to (70.00,76.50), north on x 70.00 (existing), into D109.2
    as now. Tested clear.
18. LINE_N / K103.5: R105 moves 0.01 to y 79.49; LINE_N horizontal and IN_N vertical snap to 79.49.
19. INA_O2: C129 moves 0.10 east so R110.1 and C129.1 share x 199.25; one straight.
20. Buck cluster, centre to centre: L101 0.15 west (pad 1 at x 94.00), C116 0.20 west (pads 92.60 /
    94.00), C113 0.40 south (pad 1 at y 54.10), C112 0.49 north (pad 1 at y 54.10). SW: U103.5 centre
    north on x 94.00 through C116.2 into L101.1 centre, 0.50. BST: U103.6 centre north to 51.05, 45 to
    (92.60,50.60), north into C116.1. VIN: one 0.40 line y 54.10 through U103.2, U103.3, C113.1, C112.1
    centres. To test.
21. 9 V input: D106 0.25 south (pad 1 at y 51.45); VIN_9V one 0.40 line D105.1 centre to D106.1 centre.
22. Edge bends, generic recipe (straight exit past the pad by 0.15 mm or more, then the 45): C4.1,
    R135.1, C47.1, J3.1, FB1.1, U1.13 (HSE_OUT), U104.3 (IN_P), R27.1 (ADC_SCKI_SRC). Each re-aimed and
    clearance-tested in the fix pass.
23. TP3: delete the GND trace to J106.1; TP3.1 gets its own via 1.0 mm west with a straight stub.
24. VREF at R146: the three 0.20 segments to 0.30 (DRC decides).
25. Merge the three split collinear 3V3 runs.
26. After all: gates ALL PASS, mega_audit sections loops / tees / edgebend / centre / merge at 0 except
    the registered exceptions, zones refilled, renders to `hardware/renders/`, then commit.

## 5. Parts that move

| ref | move | why |
|---|---|---|
| C132 | 0.07 west | PRE_OUT and G2_INV straight through both pads |
| C13 | 0.30 north, GND via 0.375 north | clears the 3V3 diagonal to C57.1 |
| C18 | 0.20 north | VBUS via exits straight before its bend |
| R105 | 0.01 north | matches the K103 pad row at y 79.49 |
| C129 | 0.10 east | collinear with R110.1 |
| L101 | 0.15 west | SW trunk centred on U103.5 |
| C116 | 0.20 west | SW trunk through C116.2 centre |
| C113 | 0.40 south | VIN line at the U103 pad row |
| C112 | 0.49 north | VIN line at the U103 pad row |
| D106 | 0.25 south | VIN_9V centre to centre with D105.1 |

## 6. Not checked

1. Silkscreen legibility and refdes placement (DRC silk checks are clean; no visual pass done).
2. Thermal relief on the plane connections of TH pads (KiCad defaults, not inspected).
3. OPA2197 swing, noise estimates, pot rotation sense (bench items from the pad-switch session).

## 7. Result, 2026-10-10

Owner go on items 1, 2, 4, 5, 7; item 3 partly done by the owner (VREF at R111, 9V_PLUG, I2S_DATA_SRC,
TOGGLE2 B.Cu); item 6 (TP3 ground trace) kept by owner decision, re-laid into the J106.1 centre.
Scripts: `hardware/layout/2026-10-10-prefab-fix.py` (pass 1, 86 segments) and `-fix2.py` (pass 2, 33
segments, after the audit of pass 1 found C121.1 is 1.1 x 2.8 mm and U108.1 1.32 mm wide, three via
chamfers touching their vias, C47 against C46's courtyard).

| gate | before | after |
|---|---|---|
| gates.sh | route_audit 3, pad_entry 4 | ALL PASS (DRC 9 lib mismatches, 0 unconnected) |
| loops | 1 (3V3) | 0 |
| acute joints / off-pad T | 2 / 4 | 0 / 2 registered (E0147, E0148) |
| edge bends | 18 | 0 |
| off-centre ends | 29 | 2 pass-throughs at C132, 0.07 mm each (decided) |
| mergeable splits | 3 | 0 |
| near-miss joins | 3 | 0 |
| stair-step jogs | 9 | 7, none with a clear simpler path once the pad corner is counted |

Kept, with reason: ENC_MENU_B 0.15 mm (MUX_A lane), VNEG 0.13 mm (C35 pinned by CAPM), INA_O2 0.10 mm
(C0603 1.4 mm pitch against R0603 1.5 mm pitch, no part move removes it), TOGGLE1 F.Cu dog-leg
(SW101.1 blocks the single diagonal; chamfers now 0.3 legs), LINE_P 0.6 mm straight splitting a
6.8 mm diagonal (angles gate caps 45s at 6 mm), VREF_BUF Z (1.32 mm pads leave no single diagonal),
SWCLK/SWO (E0111/E0112), 3V3 and TOGGLE1 B.Cu hops (no clear alternative), 5V 0.3 relay branch, VCAP1
neck, 9V_JACK/9V_PLUG 0.4/0.5 (owner's TP1 stubs).

Parts moved: C132 0.07 west; C13 0.30 north (GND via 0.375 north); C18 0.30 north (GND via with it);
R105 0.01 north; L101 0.15 west; C116 0.20 west; C113 0.40 south (GND via 0.40 south); C112 0.49
north (GND via to y 49.60); D106 0.25 south; TP2 0.05 north; C8 0.10 east; C45 0.10 south. C47 tried
0.20 north and reverted (courtyard against C46); the C47.1 bend was cleared by moving C45 instead.

## 8. Rail test pads, 2026-10-10

Owner: pads on the major voltage nets only. Added with the TP1 recipe (TestPoint_Pad_D1.5mm,
reference on F.Fab, value hidden), symbols next to TP4 in the root sheet and TP5 in the H7 sheet,
ERC 0. Scripts `hardware/layout/2026-10-10-testpads.py` and `-testpads2.py`.

| ref | net | pad centre | connection |
|---|---|---|---|
| TP6 | 3V3A | (150.20, 53.20) | 0.3 stub south from U4.5, the LDO output |
| TP7 | 5V | (90.80, 54.10) | 0.4 stub west from U103.1, the buck output |
| TP8 | 5VA | (155.30, 61.79) | 0.3 stub west from FB101.2 |
| TP9 | 5VA_W | (68.50, 77.80) | sits on the FB102.2 vertical, pass-through |
| TP10 | VREF_BUF | (155.75, 87.40) | is the corner of the R146.1 to U108.1 run |
| TP11 | VDDA | (105.10, 94.50) | corner of the C11.1 row to C164.1, 45 into the pad |

Skipped by decision: LDOO, ADC_VCC, ADC_VREF, VNEG, BIAS_W, VCAP1/2 (internal regulator and bias
nodes), VREF (redundant with VREF_BUF), the buck switch nodes (scope the inductor pad). First placement
put TP6/TP7 under the U4/U103 silk references and TP11 inside U1's courtyard (the pad's courtyard
radius is 1.295 mm, the tool had 1.0); fixed in pass 2 and 3. Gates ALL PASS after.
