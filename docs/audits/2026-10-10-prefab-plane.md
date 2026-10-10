# Pre-fab audit: ground return path and plane integrity, 2026-10-10

Board `hardware/cubevox.kicad_pcb` at c2635eb. Read-only. Zones filled in memory, board never saved.

Outer-layer note: no GND zone exists on F.Cu or B.Cu (only a 25.2 mm² `/5V` pour on F.Cu), so outer fill coverage is not reported.

Re-run:
```
/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 -I /tmp/prefab_plane_audit.py [sections 1-6] 2>&1 | grep -v -e Fontconfig -e wxApp -e assert
```
Scripts: `/tmp/prefab_plane_audit.py` (all sections), `/tmp/prefab_plane_webs.py` (via-row webs, necks, decap via distance and sharing), `/tmp/prefab_plane_neck.py` (web at one point). Full output of the run behind this note: `/tmp/plane/full_clean.txt`. Runtime about 15 s.

## 0. Brief premise differs from the board

The brief calls the board 2-layer with GND fill on B.Cu. The file has four copper layers.

| layer | content | evidence |
|---|---|---|
| F.Cu | signals, power, all SMT, one 25.2 mm² `/5V` pour, no GND pour | zone list |
| In1.Cu | solid `/GND` zone | zone at (56.5, 30.5)-(240.5, 174.5) mm |
| In2.Cu | solid `/GND` zone | same outline |
| B.Cu | signals and power, no GND copper (0 GND tracks) | track scan |

Stackup from the board file: F.Cu, 0.2104 mm prepreg, In1.Cu, 1.065 mm core, In2.Cu, 0.2104 mm prepreg, B.Cu, 1.6 mm total.

The checks were adapted. F.Cu tracks reference In1.Cu (0.21 mm below). B.Cu tracks reference In2.Cu (0.21 mm above). Each sample is also tested against the other inner plane. "Plane cuts" are voids in In1/In2, since no outer GND fill exists to be split.

## 1. GND zones, coverage, islands

| # | class | finding | evidence |
|---|---|---|---|
| 1.1 | OK | Both inner planes are one filled outline each. Fill 25417.0 mm² of 26817.3 mm² board area, 94.8 % coverage. In1 and In2 are identical. The 5.2 % shortfall is the 0.5 mm edge setback plus 220 holes totalling 1054.3 mm², all through-hole features aligned on both planes. | section 1 |
| 1.2 | OK | Islands per GND zone: 1 outline on In1, 1 on In2. None under 4.0 mm², so no stitch or remove candidates. Zone setting `island_removal_mode 0` deletes isolated slivers at fill. | section 1 |
| 1.3 | OK | No narrow neck. Eroding the plane by 2.0 mm leaves one piece of 19094 mm². At 3.0 mm erosion four slivers split off (50.1 mm² at (93.8, 80.1), 17.1 mm² at (205.2, 34.9), 12.0 mm² at (174.0, 70.7), 10.4 mm² at (174.4, 34.6)). These sit in dense pad fields and are not bridges between board regions. | section 1 |
| 1.5 | OK | Necks under 1 mm. Eroding both planes by 0.5 mm yields one 24042 mm² piece plus one zero-area filament at (137.8, 146.5). It is a 0.30 mm copper web between the pot pad void (132.65-137.6, 144.15-146.85) and a 3V3A pad void (137.9-140.1, 144.4-146.6). It is above the JLC 0.127 mm minimum and joins plane copper at both ends. Eroding by 0.3 mm leaves one piece, so no island results. Identical on In1 and In2. | `/tmp/prefab_plane_webs.py` |
| 1.4 | OK | F.Cu `/5V` pour is one outline of 25.2 mm². | section 1 |

## 2. Ground beneath signal and power tracks

Samples every 0.5 mm along 7319 mm of non-GND track (4223 mm on F.Cu, 3096 mm on B.Cu).

| # | class | finding | evidence |
|---|---|---|---|
| 2.1 | OK | F.Cu: 185.5 mm (4.39 %) over a void in In1. B.Cu: 204.0 mm (6.59 %) over a void in In2. Of the 389.5 mm, 387.5 mm sits at the net's own via or through-hole pad, where the signal must punch the plane. 2.0 mm sits over another net's antipad. | section 2 totals |
| 2.2 | OK | No net has a gap over 5.0 mm. Longest single gap on any net is 3.0 mm. | section 2 |
| 2.3 | OK | Only three foreign crossings, all slow signals. POT01 F.Cu 1.0 mm at (156.70, 145.33) over a pot pad antipad. TOGGLE6 B.Cu 0.5 mm at (113.97, 104.30). TOGGLE4 B.Cu 0.5 mm at (113.77, 105.50). Both over the In2 void at (111.9, 103.1)-(114.5, 108.0). | section 2 |
| 2.4 | OK | Every analog and clock net with a gap has it at its own via only. Sensitive-net totals in mm over own antipads: VREF 4.5 B / 4.0 F, LINE_N 2.5 F / 2.0 B, PRE_OUT 2.5 F / 2.5 B, INA_O2 2.0 / 2.0, I2S_BCLK 2.5 B, ADC_DOUT 2.0, ADC_SCKI 2.0, SWDIO 2.0. | section 3 |
| 2.5 | OK | B.Cu has no GND copper and does not need it. B.Cu tracks lie over solid In2 except at the gaps above. | section 2 |

Top gaps (all at the net's own through-hole pad):

| net | layer | track mm | no-GND mm | longest gap mm | start | end |
|---|---|---|---|---|---|---|
| /9V_JACK | F.Cu | 49.2 | 3.0 | 3.0 | (209.90, 41.50) | (211.00, 40.10) |
| /TRS_T | F.Cu | 78.8 | 5.0 | 3.0 | (177.42, 46.33) | (179.92, 46.33) |
| /3V3A | B.Cu | 496.4 | 49.0 | 2.5 | (139.00, 146.50) | (139.00, 144.50) |
| /ENC_MENU_SW | F.Cu | 37.5 | 4.5 | 2.5 | (176.44, 76.84) | (176.44, 78.84) |
| /ENC_MENU_A | F.Cu | 10.8 | 3.5 | 2.5 | (177.26, 63.25) | (176.44, 62.07) |
| /9V_PLUG | F.Cu | 7.4 | 5.0 | 2.5 | (222.65, 34.00) | (224.65, 34.00) |
| /9V_PLUG | B.Cu | 23.0 | 4.5 | 2.5 | (222.65, 34.00) | (220.65, 34.00) |

The per-net list of every gap is in the script output, section 2.

## 3. Plane cuts

214 voids of at least 3 mm length or 6 mm² exist per plane. The largest is 15 mm². Each has an identical twin in the other plane. None splits a plane.

| # | class | finding | evidence |
|---|---|---|---|
| 3.1 | OK | No outer GND fill exists, so no bus run splits a fill on B.Cu. Each bus run sits over a solid inner plane. | sections 0, 3 |
| 3.2 | DECIDE | POT09-POT16 via rows perforate In1 and In2 into slots. Each row has 8 vias (0.60 mm pad, 0.30 mm drill) at 0.94 to 0.98 mm pitch. The antipad radius is 0.56 mm, so neighbouring antipads overlap by 0.14 mm and no copper web survives between vias. Slot B at (145.6, 125.9)-(150.4, 130.4) is 7.7 x 1.13 mm. Slot A at (145.6, 153.1)-(150.4, 158.0) is 7.9 x 1.12 mm. Both are identical on In1 and In2. Plane copper continues around both ends, so no island or neck under 1 mm results. No other net crosses either slot. The nearest GND via is up to 14.6 mm from slot A vias and up to 5.5 mm from slot B vias. Side A, leave it, because the signals are slow pot wiper levels. Side B, add one GND via at each end of both slots, because the wipers are high-impedance and the vias cost nothing. Recommendation is side B. | `/tmp/prefab_plane_webs.py`, section 4 |
| 3.3 | OK | ENC_MENU_A/B/SW run on B.Cu at y 65.4, 66.0 and 66.7 mm from x 100.8 to 154.6 mm. Plane fill in the box (100-155, 64-68) is 100.0 % on In1 and In2. Cut width is 0 mm. F.Cu nets crossing over the run, none needing a detour because they return on In1: 3V3, 3V3A, 5V, I2C_SCL, I2C_SDA, VBUS_SENSE, and USB_DM and USB_DP (6 crossings each, flagged). Crossings are orthogonal with two ground planes between the layers. | section 3 ENC box |
| 3.6 | OK | Other via groups from the mega audit. 5 vias at (114.1, 82.5)-(115.9, 84.7), pitch 0.81 mm, no web (-0.31 mm overlap), one void 3.6 x 1.91 mm. Two ENC_MENU vias at (100.30, 80.50) and (100.95, 81.15), pitch 0.92 mm, one void 2.9 x 1.14 mm. I2S_BCLK via at (118.90, 100.55), void 2.1 x 1.13 mm. Each is identical on In1 and In2 and leaves no island or neck under 1 mm. | `/tmp/prefab_plane_webs.py` |
| 3.4 | OK | I2S/ADC runs in box (80-135, 91-108): 97.5 % fill. Three voids of 2 mm or more. The largest is 5.1 mm by 1.95 mm at (111.9, 103.1)-(114.5, 108.0), made by TOGGLE1-3, 5, 7 and XSMT vias. The others are 3.5 mm by 1.10 mm at (108.7, 102.5) and 2.1 mm by 1.13 mm at (118.3, 100.0). No I2S or ADC sample lies over a foreign void. Sensitive F.Cu nets crossing the B.Cu bus are PRE_OUT, HSE_IN and I2S_BCLK_SRC, all orthogonal and shielded by In1 and In2. | section 3 I2S box |
| 3.5 | OK | USB_DP/DM, HSE_IN/OUT, I2S_*, ADC_SCKI/DOUT, SWCLK/SWDIO, LINE/PRE/INA: zero samples over a foreign void. USB_DP/DM and HSE have no vias. MCLK and SAI nets do not exist on this board. | section 3 sensitive list |

## 4. GND stitching vias

152 GND vias plus 38 GND through-hole pads.

Nearest-neighbour spacing between GND vias:

| range mm | count |
|---|---|
| 0-1 | 2 |
| 1-2 | 34 |
| 2-3 | 38 |
| 3-4 | 24 |
| 4-6 | 16 |
| 6-8 | 15 |
| 8-10 | 8 |
| 10-15 | 8 |
| 15-25 | 7 |

Median 3.10 mm, maximum 24.00 mm.

| # | class | finding | evidence |
|---|---|---|---|
| 4.1 | OK | Stitching vias connect solidly to both inner planes with no thermal relief. | section 4 |
| 4.2 | DECIDE | Regions more than 10 mm from any GND via or GND pad exist. Counting vias and GND pads, the five largest are listed below. Counting vias only, the largest empty circle is centred at (74.0, 174.0) mm with radius 50.5 mm, on the pot panel edge. Side A, leave it, because both planes are solid and tied at 190 points. Side B, add stitches, because a 38 mm gap between plane ties is longer than one twentieth of a wavelength at 300 MHz in FR4 (about 24 mm) and the MCU runs at 480 MHz. Recommendation is side B, where free copper allows. | section 4 |
| 4.3 | OK | All 152 GND vias join both In1 and In2. 139 also touch F.Cu GND copper. 13 are plane-only stitches. None touches B.Cu (no GND copper there). Zero vias connect on one layer only. | section 4 |
| 4.4 | DECIDE | Layer-change signal vias far from a GND via: LINE_N at (195.10, 70.10) 5.0 mm and (195.10, 74.60) 4.7 mm. VREF at (223.70, 94.20) 10.9 mm and (197.75, 76.50) 6.3 mm. SWDIO at (86.53, 79.73) 4.5 mm. 118 signal vias in total, median 3.94 mm to a GND via or pad. The rest over 5 mm are static or power nets (TOGGLE, MUX_S0-3, PAD_EN, 3V3, 3V3A, 5V, POT). Side A, leave it, because the signals are audio-band or static. Side B, add a GND via within 2 mm of the LINE_N and VREF vias, because LINE_P stays on F.Cu while LINE_N changes layer and the pair is a line input. Recommendation is side B for LINE_N and VREF only. | section 4 signal-via table |
| 4.5 | OK | All I2S, ADC_DOUT, ADC_SCKI, PRE_OUT and INA_O2 layer-change vias sit 0.8 to 1.9 mm from a GND via. Examples: I2S_DATA (83.68, 107.28) 0.8 mm, ADC_DOUT (119.70, 101.20) 1.9 mm, PRE_OUT (152.00, 98.60) 1.4 mm. | section 4 |

Regions more than 10 mm from any GND via or GND pad (area from a 1 mm grid):

| region | area mm² | deepest point | radius mm | bbox mm |
|---|---|---|---|---|
| 1 | 4796 | (189.0, 99.0) | 19.0 | (56, 88)-(240, 174) |
| 2 | 741 | (203.0, 30.0) | 21.7 | (190, 30)-(240, 56) |
| 3 | 704 | (136.0, 74.0) | 20.4 | (114, 52)-(149, 88) |
| 4 | 628 | (56.0, 39.0) | 26.0 | (56, 30)-(90, 59) |
| 5 | 445 | (139.0, 30.0) | 19.1 | (130, 30)-(175, 46) |

## 5. Decoupling cap return paths

No GND pour exists on F.Cu, so no cap pad touches fill on its own layer. Every cap GND pad reaches In1 and In2 through a short trace to a via.

| # | class | finding | evidence |
|---|---|---|---|
| 5.1 | OK | 47 rail decaps on 12 ICs. None has a via in the pad. All reach a GND via 0.68 mm (C57) to 1.40 mm (C44) from the pad centre, with the trace as long as the straight distance. The via joins both In1 and In2 in every case. No decap shares its via with another pad. | section 5 table |
| 5.2 | FIX | C127 (100 nF, `/5VA`) is the only cap on U105 pin 8 at (229.10, 81.90). Its power pad at (226.80, 78.50) is 4.10 mm straight and 6.54 mm of trace from the pin. The loop is 7.34 mm. Every other IC supply pin has a small cap loop of 2.34 to 5.24 mm. Fix by placing C127 within 2 mm of pin 8 with a GND via within 1 mm. | section 5 best-cap line |
| 5.3 | OK | Best small-cap loop per supply pin (power path plus GND trace), mm: U1 pins 100 2.34, 50 2.39, 11 2.95, 27 3.05, 6 3.77, 75 3.83, 21 4.04. U8 pins 3 3.39, 4 4.08, 1 4.16. U6 pins 20 2.82, 8 3.38, 18 5.24. U101 pin 24 3.00. U103 pin 3 2.85. U104 pin 8 2.70. U106 pin 8 3.00. U107 pin 3 3.02. U108 pin 5 2.60. U4 pin 1 3.17. | section 5 |
| 5.4 | DECIDE | Eight more rail caps have no IC supply pin within 12 mm (C115, C117, C134, C160, C18, C19, C29, C30). Pad-to-via distance is 0.85 to 1.44 mm for six. C117 (10 uF, 5VA) is 1.60 mm, via at (156.00, 64.01). C160 (10 uF, VREF) is 1.84 mm, via at (161.00, 97.00). Both exceed 1.5 mm. Side A, leave, because both are bulk caps with no HF duty. Side B, move the vias to 1 mm. Recommendation is side A. No via is shared. Largest distance on the whole board is 1.84 mm. | `/tmp/prefab_plane_webs.py` |
| 5.5 | DECIDE | IC ground pins with trace longer than straight distance to a via. U8 DGND pin 5 at (126.80, 101.85) has 3.07 mm of trace against 2.34 mm straight. U6 AGND pin 9 at (75.53, 108.58) has 2.37 mm against 1.23 mm. MCU VSS pins 10, 74 and 99 have 2.42, 2.29 and 2.63 mm of near-straight trace. Side A, leave all, because the planes sit 0.2104 mm below the pads and the traces are short. Side B, re-route U8 pin 5 and U6 pin 9 to their nearest vias. Recommendation is side B for those two analog-IC pins and leave the MCU pins. Strap pins (MD0, MD1, FMT, DEMP, FLT, SCK) carry no return current and are excluded. | section 5 IC ground table |

## 6. Analog ground versus the buck converters

Two buck converters exist, U103 (AP63205, 5 V, inductor L101) and U3 (AP63203, 3.3 V, inductor L1). Both are synchronous with integrated FETs, so no external diode or FET exists.

| # | class | finding | evidence |
|---|---|---|---|
| 6.1 | OK | One GND piece on each inner plane carries the buck, MCU U1, DAC U6, ADC U8 and all op-amps. No moat or split. | sections 1, 6 |
| 6.2 | OK | U103 input hot loop is Cin+ pad, VIN pin 3 at (94.95, 54.10), GND pin 4 at (94.95, 51.90), Cin- pad. GND pin 4 reaches a via at (94.95, 51.00) over 0.90 mm of trace. C113 (100 nF at (97.00, 52.70)): pad polygon 3.69 mm², plane-image area 1.26 mm². C112 (22 uF): 12.24 mm² and 2.43 mm². U3 with C20 (22 uF): 6.57 mm² and 1.48 mm². Plane-image area is trace plus plane path length times the 0.2104 mm prepreg. | section 6 loop tables |
| 6.3 | OK | Switch-node loop (SW pin, inductor, output cap, GND pin). U103 via C114: 13.15 mm² (C115 further out: 44.42 mm²). U3 via C22: 13.99 mm². SW copper is 5.90 mm (U103) and 5.80 mm (U3) of 0.50 mm track on F.Cu with no vias, about 2.9 mm² each plus 0.70 mm² of pin pad. | section 6 |
| 6.4 | OK | No analog return crosses either buck loop. The loop region (both bucks, pad polygons plus 1 mm) is x 84.8-104.5 mm, y 41.8-75.5 mm. No analog track lies inside it. Closest are OUT_TIP on F.Cu (x 80-82, y 47-74) at 3.0 mm from the region and 4.1 mm from C21 at (85.8, 70.8), OUT_AC 5.2 mm, BUF_R 7.4 mm, BUF_OUT 11.2 mm. Their returns run under the tracks on In1, away from the loops. The ADC, DAC and op-amps lie 41 to 141 mm from U103. | section 6 |
| 6.5 | OK | The 9 V input return runs in the solid planes under VIN_9V (track extent x 94-234 mm, y 43-56 mm) from J106 at (225.0, 37.0) to U103. It passes J101 at (175.0, 45.7) and U4 at (151.5, 48.5). Ripple amplitude on that path was not quantified here. | section 6 rail extents |

## Summary

| class | count |
|---|---|
| FIX | 1 |
| DECIDE | 5 |
| OK | 25 |

FIX: 5.2. DECIDE: 3.2, 4.2, 4.4, 5.4, 5.5.
