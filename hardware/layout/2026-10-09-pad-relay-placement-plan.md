# Pad relay K103 and gain divider: placement plan

Status 2026-10-09: schematic pin swap committed (c4788e2). Placed per §2 with
`layout/2026-10-09-pad-relay-place.json` (foreman edit, then netlist sync; parity 0):
K103, D110, Q102, R160, R161, R159 added, R116 moved and renetted. Removed with the
move: the old G2_RG run (235.25, 78.5)→(237, 81.2), the G2_INV stub at y 78.5, and
the old LINE_N run C125.1→(194.3, 79.1)→(194.7, 79.5), which K103 pad 5 now sits on.
R105.1→pad 5 at y 79.5 stays. No new traces. Gates: DRC 9 lib mismatches, 17 open
connections (the routing pass), every other gate PASS.
Routing 2026-10-09 (owner, in KiCad): everything except PAD_EN, with the P leg on F.Cu
round the north (LINE_P y 70.8) and south (IN_P y 92.0) of U104 instead of §4.3, 5V on
F.Cu down x 159.3 and along y 84.23 instead of §4.5, and 5VA re-fed round the east at
x 229.1. Lead fixes after the check: FB101.1 and VREF junctions snapped to exact pad
coordinates, VREF via to (223.7, 94.2), LINE_P corner at (215.54, 72.16) re-laid as a
Y off the x 214.1 vertical, G2_INV split as a straight-through vertical with the R159
branch at 45 from (232.5, 70.5), GND vias (187.3, 70.5) and (184.6, 78.0), and the 3V3
feed to R138–R141 restored with a B.Cu hop at x 173.7 (vias y 82.9 and 85.6) across
the 5V lane. PAD_EN moved to U1 PE11 pad 41 (schematic + netlist sync); MUX_S0–S3
escape vias re-laid to (115.9, 81.5) (115.3, 83.5) (114.1, 84.7) (114.7, 84.1) so PAD_EN
leaves pad 41 east on F.Cu at y 82.5 with 0.3 to each neighbour and a clear via spot
near (117.2, 82.5). §4.6 below is superseded; only the B.Cu lane east of x 117 stands.
Gates after the fixes: every gate PASS except DRC's one open connection, PAD_EN.
Pictures: `hardware/renders/2026-10-09-pad-relay-routed-input-corner.png`,
`-routed-pot-corner.png`, `-mux-escapes-top.png`, `-mux-escapes-bottom.png`,
`-3v3-hop-top.png`; placement stage: `-placed-input-corner.png`,
`-placed-pot-corner.png`, `-placed-top.png` (plan drawings: `-plan-input-corner.png`,
`-plan-5v-paden.png`).
Corridors were checked with a 0.1 mm grid path search over the real copper
(`/tmp/cv_route/corridor.py`, scratch) at 0.2 trace / 0.2 clearance, via 0.6 to foreign
copper, via-via 0.8. The board rule minimum is 0.15; the brief uses 0.2.

## 1. Why here

1. K103 shorts R102 (LINE_P–IN_P, x 211.5) and R105 (LINE_N–IN_N, x 198.5). U104 sits
   between them with IN_P on its east pad column and IN_N on its west column, so one leg
   of any single DPDT relay crosses U104. Under U104 is ruled out: C158 bridges A1_FB (east)
   to RG_MID (west) and the LINE_P loop would have to cross A1_FB on F.Cu.
2. The west patch x 186–197, y 78–88 is empty on F.Cu. Next to it, x 182.6–188.9,
   y 62–79 is empty on both layers except the 5VA B.Cu line at y 68.33. That column takes
   the driver, the 5 V arrival and the PAD_EN arrival.
3. 5 V ends at FB101 (157.6, 60.2). 5VA runs as one B.Cu wall from (135.65, 96.4) round to
   (158, 71) and on at y 68.33 to x 198.4, and 3V3 runs a F.Cu loop round the encoder
   (x 159.97 / y 59.3 / x 182.0 / y 79.6–82.0). Every east–west signal hops those on the
   other layer; the plan names each hop.

## 2. Parts and positions (mm, rotation KiCad degrees)

| Ref | Part | Centre | Rot | Pads (net) |
|---|---|---|---|---|
| K103 | G6K-2F-Y DC5, `RELAY-SMD_G6K-2F-X-XX` | (191.30, 83.00) | 0 | 1 (187.5, 86.51) 5V · 2 (190.7, 86.51) nc · 3 (192.9, 86.51) LINE_P · 4 (195.1, 86.51) IN_P · 5 (195.1, 79.49) **LINE_N** · 6 (192.9, 79.49) **IN_N** · 7 (190.7, 79.49) nc · 8 (187.5, 79.49) PAD_COIL |
| D110 | 1N4148W SOD-123F | (184.60, 82.50) | 90 | 1 (184.6, 84.23) 5V · 2 (184.6, 80.77) PAD_COIL |
| Q102 | MMBT3904 SOT-23 | (185.60, 75.50) | 180 | 1 B (184.6, 74.55) · 2 E (184.6, 76.45) GND · 3 C (186.6, 75.5) PAD_COIL |
| R160 | 1k | (185.35, 72.50) | 180 | 1 (186.1, 72.5) PAD_EN · 2 (184.6, 72.5) PAD_BASE |
| R161 | 100k | (185.35, 70.50) | 0 | 1 (184.6, 70.5) PAD_BASE · 2 (186.1, 70.5) GND |
| R116 | 470R, **moves** from (234.5, 78.5) | (235.00, 74.75) | -90 | 1 (235.0, 74.0) G2_BOT · 2 (235.0, 75.5) G2_RG |
| R159 | 1k, new | (233.40, 74.75) | -90 | 1 (233.4, 74.0) G2_INV · 2 (233.4, 75.5) G2_RG |

Courtyards: K103 185.96–196.64 × 78.14–87.86 (0.36 to R110, 0.64 to R149). D110
183.46–185.74 × 79.93–85.07. Q102 183.8–187.4 × 73.7–77.3. R160 / R161 183.9–186.8.
R116 / R159 73.3–76.2, 0.1 between them, 3.7 below the pot courtyard.

Schematic change needed: swap K103 pins 5 and 6 (pin 5 = LINE_N, pin 6 = IN_N). Both are
the same closed contact; the swap puts LINE_N on the pad next to R105 so the N leg routes
without a crossing. Nothing else in the schematic changes.

## 3. New vias

| Net | Via | Purpose |
|---|---|---|
| 5V | (183.2, 66.0) | B.Cu feed arrives, F.Cu column down to D110.1 |
| PAD_EN | (187.3, 72.5) | B.Cu lane y 72.9 up to R160.1 |
| GND | (187.3, 70.5) | R161.2 |
| GND | (184.6, 78.0) | Q102.2 emitter |
| IN_P | (209.3, 80.4) | drop from the IN_P stub at (209.3, 79.3) |
| LINE_P | (215.0, 79.9) | branch from the LINE_P corner (213.7, 79.3) |
| LINE_P | (192.9, 88.6) | up into K103.3 from below |
| IN_P | (195.1, 88.6) | up into K103.4 from below |
| PAD_EN | (89.9, 88.2) (107.3, 81.3) (117.1, 82.2) (153.9, 83.9) (158.4, 83.8) | MCU side, see §4.6 |

GND return vias: none of these nets is in `classification.critical`; no return vias.

## 4. Corridors (every turn two 45s, pads entered on the centre line)

### 4.1 Gain divider (F.Cu, pot area)
1. G2_BOT: RVGAIN1.3 (235.0, 68.5) straight down to R116.1 (235.0, 74.0).
2. G2_INV: stub east from the existing x 231.77 vertical at y 74.0 to R159.1 (233.4, 74.0).
3. G2_RG: R159.2 (233.4, 75.5) east to R116.2 (235.0, 75.5); on down x 235.0 to 77.0, 45 to
   (237.0, 79.0), down into C159.1 (237.0, 81.2). The old G2_RG run and the old G2_INV
   stub at y 78.5 are already removed (placement pass). C132 stays.

### 4.2 N leg (F.Cu only, no vias)
1. LINE_N: C125.1 (191.5, 72.16) east to 194.7, 45 to (195.1, 72.56), down x 195.1 into
   K103.5 top edge (78.44). R105.1 (197.75, 79.5) west into K103.5 (right edge 195.6):
   the existing segment (197.75, 79.5)–(194.7, 79.5) already does this and stays. The old
   vertical at x 194.3 and its 45s are already removed (placement pass).
2. IN_N: R105.2 (199.25, 79.5) down to 80.3, 45 to (198.55, 81.0), west to 193.6, 45 up
   to (192.9, 80.3), into K103.6 from below. Lane y 81.0 clears pad 5 by 0.36 and R110 by 1.4.
   The existing IN_N runs to C124 and U104.5 stay.

### 4.3 P leg (B.Cu under U104, two vias per net)
1. IN_P: stub (209.3, 79.3) down to via (209.3, 80.4). B.Cu west along y 80.3 to 206.3,
   45 to (203.7, 82.9), west to 199.5, 45 to (195.1, 87.3), down to via (195.1, 88.6),
   F.Cu stub up into K103.4.
2. LINE_P: branch at (213.7, 79.3), 45 to (214.3, 79.9), east to via (215.0, 79.9). B.Cu
   45 to (214.6, 79.5), west along y 79.5 to 204.9, 45 to (202.3, 82.1), west to 197.9,
   45 to (192.9, 87.1), down to via (192.9, 88.6), F.Cu stub up into K103.3.
3. Lane pitch 0.8; the paired 45s are offset 0.42 and 0.57 perpendicular. The 5VA via
   (201.1, 81.4) sits 0.7 from the LINE_P lane and 1.3 from its diagonal; the 5VA B.Cu
   line y 81.4 is 0.7 from LINE_P. Both planes are solid GND here.

### 4.4 Driver column (F.Cu)
1. PAD_BASE: x 184.6 straight from R161.1 (70.5) through R160.2 (72.5) to Q102.1 (74.55).
2. PAD_COIL: Q102.3 (186.6, 75.5) down to 77.6, 45 to (187.5, 78.5), into K103.8 top.
   D110.2 (184.6, 80.77) up to 80.3, 45 to (185.4, 79.49), east into K103.8 from the west.
3. 5V: via (183.2, 66.0) down x 183.2 to 83.5, 45 to (183.9, 84.23), into D110.1; then
   D110.1 down to 85.5, 45 to (185.6, 86.5), east into K103.1. x 183.2 clears 3V3 (x 182.0)
   by 1.1 and the column pads (x 184.6) by 0.85.
4. GND: R161.2 east 1.2 to via (187.3, 70.5). Q102.2 down 1.55 to via (184.6, 78.0).

### 4.5 5V feed (B.Cu, 1 via)
From the existing 5V via (157.6, 59.38): east along y 59.4 to 181.7, 45 to (183.2, 60.9),
down to via (183.2, 66.0). Clears ENC_MENU_A (y 61.2) by 1.8, the GND via (180.94, 60.5)
by 1.1 on the lane and 1.3 on the 45, the 5VA line y 68.33 by 2.3 at the via. This stays
north of the 5VA wall; the F.Cu column in §4.4 is the hop across it.

### 4.6 PAD_EN (U1 PB8, pad 95, 6 vias, about 100 mm)
PB8 stays. Every other free U1 pin (PE11, PB12–PD15 top row, PA8/PA10, PB4/PB6/PB9) is
fenced at 0.5 pitch by its neighbours' escapes, with no via room before the MUX_S vias
at x 115–116; the search found no path from any of them without re-laying the MUX bus
and VA_SENSE.
1. F.Cu: pad 95 (88.45, 87.5) east under the body to via (89.9, 88.2).
2. B.Cu: (91.1, 88.6) (92.7, 90.0) (96.3, 90.0) (100.1, 86.2) (102.5, 86.1) (107.2, 81.4)
   to via (107.3, 81.3). This threads between GND stitch vias under U1's south edge; the
   brief may move two of them to straighten it.
3. F.Cu: y 81.3 from 107.3 to 116.0, between I2C_SCL (80.6) and MUX_S0 (82.0), 45 to
   via (117.1, 82.2).
4. B.Cu: y 82.4 from 117.3 to 148.3, 0.8 below VA_SENSE, 45 to y 83.9, to via (153.9, 83.9).
5. F.Cu hop over the 5VA wall: y 83.8 from 153.9 to via (158.4, 83.8), under R151's
   courtyard edge, 0.7 below the 5VA F.Cu stub at y 83.1.
6. B.Cu: 45 to (169.3, 72.9), y 72.9 east under the encoder (EH pads 1.0 above, E/D pads
   3.9 below) to 186.9, 45 to via (187.3, 72.5), F.Cu 1.2 west into R160.1.

## 5. Parts that may move
R116 (named above). GND stitch vias under U1 named in the brief for §4.6.2. Everything
else stays, including R102, R105, C118, C158, U104 and the MUX bus.

## 6. Order of work (routing pass, after the owner's go)
1. Schematic: swap K103 pins 5/6; commit the schematic.
2. PCB: update from schematic (brings K103, D110, Q102, R160, R161, R159 in and renets
   R116). Place per §2. Render, owner looks.
3. Gain divider §4.1, then N leg §4.2, P leg §4.3, column §4.4, 5V §4.5, PAD_EN §4.6.
   DRC after each group.
4. Gates on the saved file: `hardware/tools/gates.sh`. Renders to `hardware/renders/`.

## 7. Stop clause
A spot under 0.2 clearance or a via under 0.6 from foreign copper is a defect in this
plan. Stop, report the spot, never squeeze.
