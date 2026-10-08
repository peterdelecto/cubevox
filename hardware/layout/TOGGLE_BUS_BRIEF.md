# TOGGLE bus: U1 pin reassignment and east-side re-lay

Owner-approved direction 2026-10-08. As built (D0381, ledger 0479): sections 4.2-4.14 state the
copper on the board, including the lead-approved corrections to 4.9, 4.11 and 4.13. Replaces the failed grid-search attempt that left
TOGGLE1-8 unrouted (commit 2d5c7bc). All coordinates mm, board origin as in cubevox.kicad_pcb.

## 1. Why

- U1's east edge interleaves TOGGLE3-8 with MUX_S0-3, VA_SENSE and XSMT on 0.5 mm rows.
  Those five already use every exit, so the toggles cannot reach the one F.Cu corridor
  south (x 110-113, between the 3V3 via at (109.5,91.0) and R16/R30).
- B.Cu south of U1 is walled by the SAI/ADC lines (y 94.35-102.3, x 88-128) and XSMT
  B.Cu at y 106.88 (x 89.6-112). Only F.Cu lanes can cross that band.
- The 3V3 B.Cu vertical (109.5, 71.97-91.0) is the sole feed of the island {U1.27, C58,
  C3, FB1/VDDA, U8.4, C46, C47, R134-R137, SWBOOT1, x58 rail}. It stays.
- West edge free pins face J3 (Tag-Connect) and the U6 input fan; no clean path south.
- The trunk band on B.Cu is empty: y 102.3-107 from x 113 east has only GND vias at
  (122.9,103.8) (123.6,102.9) (125.5,103.8) (130.78,104.38) (131.62,105.5) (134.28,106.88)
  (165.78,103.4) and the 3V3A feed; y 107.2-111.0 from x 105 east has only the 3V3A
  B.Cu vertical at x 142.62.

## 2. Schematic: U1 pin reassignment (h7core_block.kicad_sch + top sheet ports)

| pin | port | old net   | new net   |
|-----|------|-----------|-----------|
| 28  | PA4  | TOGGLE3   | TOGGLE8   |
| 29  | PA5  | TOGGLE4   | TOGGLE6   |
| 30  | PA6  | MUX_S2    | TOGGLE4   |
| 31  | PA7  | MUX_S3    | TOGGLE1   |
| 32  | PC4  | MUX_S1    | TOGGLE2   |
| 33  | PC5  | MUX_S0    | XSMT      |
| 34  | PB0  | TOGGLE5   | TOGGLE3   |
| 35  | PB1  | VA_SENSE  | VA_SENSE (unchanged) |
| 36  | PB2  | TOGGLE6   | TOGGLE5   |
| 37  | PE7  | TOGGLE7   | TOGGLE7   |
| 38  | PE8  | TOGGLE8   | MUX_S2    |
| 39  | PE9  | XSMT      | MUX_S3    |
| 40  | PE10 | NC        | MUX_S1    |
| 41  | PE11 | USER_LED  | NC        |
| 42  | PE12 | NC        | MUX_S0    |
| 92  | PB6  | TOGGLE1   | NC        |
| 93  | PB7  | NC        | USER_LED  |
| 98  | PE1  | TOGGLE2   | NC        |

After the edit: export netlist (`kicad-cli sch export netlist`), update pad nets on the
board (pcbnew API, pad.SetNet) to match, and prove parity with
`kicad-cli pcb drc --schematic-parity` (expect only the 9 lib_footprint_mismatch and the
pre-existing 70 field warnings). Update hardware/PINMAP.md and the firmware pin table
(firmware/cubevox: TOGGLE1-8, MUX_S0-3, XSMT, USER_LED).

## 3. Geometry rules (measured on the board)

Trace 0.2 signal, 0.3 power. Clearance 0.2. Via 0.6/0.3. So: trace centre to trace centre
>= 0.4; via centre to other-net trace centre >= 0.6; via centre to via centre >= 0.8.
0/45/90 only. Pads entered straight on. No GND return vias (none of these nets is critical).

U1 east pads: x 103.55 centre, 1.6 x 0.3, span x 102.75-104.35. Rows (y): 28=89.0,
29=88.5, 30=88.0, 31=87.5, 32=87.0, 33=86.5, 34=86.0, 35=85.5, 36=85.0, 37=84.5,
38=84.0, 39=83.5, 40=83.0, 41=82.5, 42=82.0.

## 4. East-side re-lay, part by part

### 4.1 Rip up
MUX_S0-3 F.Cu exits and B.Cu staircase west of x 118.55 (keep the B.Cu runs east of
there at y 89.5/90.15/90.9/91.55 and the F.Cu descents at x 141.8-143.75). VA_SENSE F.Cu
(103.55-114.5, 85.5), its via (115.6,86.5), B.Cu (115.6-116.7, 86.5-81.9), C163 and its
GND via (114.5,88.9). XSMT F.Cu (pin 39 to R16 and x 112 down to 106.88) and the via
(112.0,106.88); keep the B.Cu run (89.6-112.0, 106.88) and extend it. USER_LED F.Cu to R3.
R16 and its GND via (114.4,92.0). C138, its GND via (109.27,105.5) and ADC_VINR F.Cu.
The old TOGGLE1/2 west stubs stay (they end at the switch clusters; they are reached from
the trunk).

### 4.2 Nine F.Cu lanes from U1's east rows (the corner)
Rows 89.0 (28), 88.5 (29), 88.0 (30), 87.5 (31), 87.0 (32), 86.5 (33), 86.0 (34),
85.0 (36), 84.5 (37) exit east on F.Cu and bend 45 degrees down into lanes:

| row | pin | net     | lane x | role |
|-----|-----|---------|--------|------|
| 89.0 | 28 | TOGGLE8 | 110.1 | west comb, trunk row 103.1 |
| 88.5 | 29 | TOGGLE6 | 110.5 | west comb, row 104.3 |
| 88.0 | 30 | TOGGLE4 | 110.9 | west comb, row 105.5 |
| 87.5 | 31 | TOGGLE1 | 111.3 | runs on to y 110.0, then west |
| 87.0 | 32 | TOGGLE2 | 111.7 | runs on to y 110.6, then west |
| 86.5 | 33 | XSMT    | 112.1 | runs to 105.88, jogs to via (113.1,106.88) |
| 86.0 | 34 | TOGGLE3 | 112.5 | east comb, row 106.1 |
| 85.0 | 36 | TOGGLE5 | 112.9 | east comb, row 104.9 |
| 84.5 | 37 | TOGGLE7 | 113.3 | east comb, row 103.7 |

Corner: row r (0..6, y = 89.0 - 0.5r) runs east to x 109.6 + 0.4r, 45 degrees to
(110.1 + 0.4r, 89.5 - 0.5r), then south. Row 85.0 bends from (111.9, 85.0) to (112.9, 86.0);
row 84.5 from (112.8, 84.5) to (113.3, 85.0). Diagonal spacing 0.64 mm or more.

Lane 110.1 clears the 3V3 via (109.5,91.0) by 0.2 and the 3V3 F.Cu vertical (109.5,
91-99.6) by 0.35. Lane 113.3 needs R30.2 pad edge >= 113.6: shift R30, C55 and the
MUX1_SIG_ADC via/vertical/GND via east by 0.7 (R30 pads to x 114.2/115.85, C55 to x 114.2,
via (113.5,93.2) to (114.2,93.2), GND via (113.5,98.6) to (114.2,98.6), POT_MUX_OUT via
(115.15,93.2) to (115.85,93.2)). Re-jog PRE_OUT: (116.95,94.0) straight down x 116.95 to
y 98.7, 45 degrees to (116.25,99.4), straight into R119.1 at (116.25,100.0).

### 4.3 Pin 27 3V3 stub, C58 and C3
Rows pass 0.5 above the stub. Move C58 to (105.6, 90.48) and C3 to (106.9, 90.775)
(both 0.5 lower than now, C58 0.5 east, C3 0.5 east). Stub: pin 27 (104.35,89.5) east to
104.6, 45 degrees to (105.1,90.0), straight into C58.1 (105.6,90.0), on to C3.1 (106.9,90.0),
to (109.05,90.0), 45 degrees to (109.5,90.45), to the via (109.5,91.0). GND vias of C58/C3
move with them: (105.6,91.75) and (106.9,92.35).

### 4.4 VA_SENSE (pin 35) goes inward and over the top of the 3V3 wall
F.Cu pad inner end (102.75,85.5) west to via (102.1,85.5). B.Cu north at x 102.1 to y 83.6,
45 degrees to (103.1,82.6), north at x 103.1 to y 71.1, east at y 71.1 to x 116.7, south at
x 116.7 to y 81.9, then the existing (116.7,81.9)-(117.0,81.6)-east run. Clearances: GND
via (102.35,78.5) 0.35; 3V3 via (109.5,71.97) 0.47. Verify B.Cu at y 71.1, x 103-117 is clear.
C163 (VA reservoir, owner decision D0188-D0190 keeps it at the MCU end) sits at (115.6,73.5)
rot -90, west of the x 116.7 vertical, because J117's OLED courtyard starts at x 117.84.
The vertical splits at a tap via (116.7,72.8); F.Cu runs west into C163.1 (115.6,72.8).
C163.2 (115.6,74.2) drops to a GND via at (115.6,75.0).

### 4.5 MUX_S0-3 (pins 38, 39, 40, 42) and their B.Cu lanes
Rows 84.0, 83.5, 83.0, 82.0 exit east over the corner. Vias and B.Cu lanes:
- pin 38 MUX_S2: row 84.0 to x 113.5, 45 degrees to via (114.1,84.6); B.Cu south at 114.1
  to y 91.55, east to join the existing S2 run.
- pin 39 MUX_S3: row 83.5 to x 114.2, 45 degrees to via (114.7,84.0); B.Cu south at 114.7
  to y 90.9, east.
- pin 40 MUX_S1: row 83.0 to via (115.3,83.0); B.Cu south at 115.3 to 90.15, east.
- pin 42 MUX_S0: row 82.0 to via (115.9,82.0); B.Cu south at 115.9 to 89.5, east.
Each lane passes the via west of it at 0.2 edge clearance. VA's B.Cu vertical at 116.7
clears lane 115.9 by 0.6 and the via (115.9,82.0) by 0.4.

### 4.6 XSMT
Lane 112.1 south to (112.1,105.88), 45 degrees to the via (113.1,106.88) on the extended
B.Cu run (89.6-113.1, 106.88). R16 (pull-down) moves to (98.5,106.0) rot 0: pad 1 stub
straight down to a tap via (97.675,106.88) on the same B.Cu run, which splits there; pad 2
GND via at (99.325,105.0).

### 4.7 USER_LED on pin 93 (west)
Pin 93 (88.45,86.5) exits west on F.Cu into R3.1. R3 at (85.5,86.5) rot 180 (pad 1 east at
86.325, pad 2 at 84.675). D1 stands at (82.93,87.25) rot 90: pad 1 (anode net USER_LED_A) at
(82.93,86.5) on the same row, pad 2 GND at (82.93,88.0) to a GND via at (82.93,88.8). Body
boxes clear U1 and each other by 0.33; D1 stays vertical because inline it would land on
the NRST via (81.12,86.25).

### 4.8 The two via combs and the six trunk rows (B.Cu, y 103.1-106.1)
West comb, vias at x 109.3: lane 110.1 jogs from (110.1,102.3) to (109.3,103.1);
lane 110.5 from (110.5,103.1) to (109.3,104.3); lane 110.9 from (110.9,103.9) to (109.3,105.5).
East comb, vias at x 113.9: lane 113.3 from (113.3,103.1) to (113.9,103.7);
lane 112.9 from (112.9,103.9) to (113.9,104.9); lane 112.5 from (112.5,104.7) to (113.9,106.1).
Rows run east from each via: 103.1 T8, 103.7 T7, 104.3 T6, 104.9 T5, 105.5 T4, 106.1 T3.
West rows pass the east vias at 0.6 centre spacing. Top via edge 102.8 clears ADC_SCKI
B.Cu (102.3) by 0.4; bottom via edge 106.4 clears XSMT B.Cu (106.88) by 0.38.

### 4.9 T3 drop and the step
T3 (row 106.1) drops at x 115.9: B.Cu south to y 115.0, west into SW103 pad 2 (112.5,115.0).
The other five rows step down 45 degrees to 107.9-110.3 between x 114.6 and 122
(bottom stepping row 105.5 starts its diagonal at x 117.0, each row above starts 0.6
further east, all end 4.8 lower; starts T4 117.0, T5 117.6, T6 118.2, T7 118.8, T8 119.4,
diagonal spacing 0.85). This passes west of the ADC GND vias at x 122.9-134.3.
Stepped rows: 107.9 T8, 108.5 T7, 109.1 T6, 109.7 T5, 110.3 T4. Row 110.3 clears the
switch pad tops (111.21) by 0.81. Drops: T4 at 138.45 from 110.3, T5 at 162.45 from 109.7,
T6 at 186.45 from 109.1, T7 at 210.45 from 108.5, T8 at 234.45 from 107.9; each B.Cu south
to y 115.0 then west into pad 2 of its switch. Drop columns measured clear on B.Cu.

### 4.10 3V3 bar hop (F.Cu bar at y 107.55 crossed by lanes 111.3 and 111.7)
Vias at (109.3,107.55) and (114.3,107.55); the bar is B.Cu between them. Clears the west
comb via (109.3,105.5) by 2.05, the XSMT via (113.1,106.88) by 1.37, T3's drop at 115.9
by 1.2.

### 4.11 TOGGLE1 and TOGGLE2 west
Lane 111.3 (T1) via at (111.3,110.0); lane 111.7 (T2) jogs from (111.7,109.0) 45 degrees to
(112.5,109.8), then straight down to the via (112.5,110.6), clearing T1's via by 0.59. B.Cu rows west: T1 at y 110.0 to x 93.0, 45 degrees to (92.4,110.6), on at
110.6 to x 68.3, via, F.Cu (68.3,110.6)-(68.3,113.7)-(67.0,115.0)-(66.0,115.0) joining the
existing SW101 stub. T2 at y 110.6 to x 94.0, south at x 94.0 to y 115.0, west into SW102
pad 2 (88.5,115.0). Row 110.6 clears U6's GND vias (75.53/81.27, 109.81) by 0.39 and the
3V3A B.Cu at x 67 by 1.0 at the via.

### 4.12 3V3A crossing at x 142.62
Move the vertical: the B.Cu bar now starts at (144.5,106.28) (its old 143.42 end is gone), south to via (144.5,107.0),
F.Cu south to via (144.5,111.4), B.Cu south at 144.5 to y 131.08, west to the via
(142.625,131.08). Delete the old vertical (142.62, 107.08-131.08) and the elbow to 143.42.
F.Cu at 144.5 clears MUX_S0 (143.75) by 0.5. Hop vias clear rows 107.9 and 110.3 by 0.5/0.7.

### 4.13 C138 (ADC_VINR cap)
At (121.2,108.0) rot -90, straddling the 3V3 bar. Pad 1 (121.2,106.41) takes ADC_VINR as a
straight stub from U8.14 (121.2,104.45); pad 2 (121.2,109.59) drops to a GND via at
(121.2,111.2). The bar passes between the pads with 0.25 clearance. A rot 0 pose under U8
puts pad 2 on the 3V3 feed vertical (124.6, 102.9-107.55). Body gap to U8 is 0.02, the one
new tight pair.

## 4.14 Leftover from the last pass
The 5VA B.Cu segment (198.368,68.332)-(157.6,68.33) drifted 0.002 mm; the corner is now
(198.37,68.33), so the diagonal from (198.95,67.75) stays exactly 45 degrees.

## 5. Order of work
1. Schematic pin swap, netlist, pad nets, parity DRC. Commit.
2. Rip-up list 4.1, part moves (4.3, 4.5 R30/C55 shift, 4.6 R16, 4.7 R3/D1, 4.4 C163,
   4.13 C138). Scratch copy, DRC, render, parent review.
3. Corner and lanes (4.2), combs and rows (4.8), step and drops (4.9), bar hop (4.10),
   T1/T2 (4.11), 3V3A (4.12), MUX_S (4.5), VA (4.4), XSMT (4.6), USER_LED (4.7).
   Scratch copy after each group, DRC, render, parent review before the real apply.
4. Gates on the saved file: DRC with parity (9 lib only, unconnected 0), via_audit_strict
   0 true orphans, pad_entry_check 0, route_audit 0 micro / 0 mid-track joins, angle check
   0 off-angle and 0 diagonals over 20 mm, renders to hardware/renders/cubevox-toggle-*.png,
   RUNLOG row, declarations and ledger per AGENT_BRIEF.md.
5. Commits with explicit pathspecs: (a) schematic+pinmap+firmware, (b) board re-lay.
   No push. No attribution lines.

Anything here that measures short of 0.2 clearance when applied is a defect in this brief,
not a licence to squeeze: stop, report the exact spot, and wait.
