# Pre-fab BOM, CPL and assembly audit, 2026-10-10

Board `hardware/cubevox.kicad_pcb` at commit c2635eb (4 copper layers, JLC04161H-7628, so the v2 gerber zip correctly lists In1 and In2). Read-only. Scratch files in `/tmp/bomaudit/`; nothing written under `hardware/`.

## Method

1. Netlist and positions exported fresh to `/tmp`: `kicad-cli sch export netlist`, `kicad-cli pcb export pos --format csv --units mm --side front --exclude-dnp`.
2. `hardware/tools/make_bom.py` run on those two files into `/tmp/bomaudit/bom.csv` and `cpl.csv`. Result: 73 BOM lines, 222 placed parts, 73 distinct LCSC numbers, no missing LCSC, BOM and CPL designators equal.
3. LCSC package, stock and tier from `jlc_get_part` for all 73 numbers; `jlc_stock_check` repeated for RK09D1130C2P, STM32H743VIT6, EEEHP1E220P and ANR5040T3R9M (same stock as `jlc_get_part`). Pin names from `jlc_get_pinout` for 18 parts. Board geometry from the KiCad Python API.
4. How `make_bom.py` works: the LCSC number is the schematic field `LCSC`. Rows group by LCSC number. Skipped: exclude-from-BOM, DNP, and the hard-coded set J101, J106, J108. CPL takes `Ref, PosX, PosY, Side, Rot` straight from the pos export. There is no rotation offset table anywhere in the script (F3).

## Findings

| # | Class | Finding |
|---|---|---|
| F1 | FIX | `hardware/exports/cubevox-netlist.net` is stale (written 2026-10-09 01:07). Run through `make_bom.py` it yields a BOM with K102 only, D109 only, Q101 only and no R159, R160, R161. The v3 export must re-export the netlist and the pos file first. |
| F2 | FIX | CPL carries 75 negative rotations (for example -90 on U6, U106, U107, Y1, D107, D108, C139, C159). JLC's range is 0 to 359. `write_cpl` should apply `% 360`. v2 had the same form. |
| F3 | FIX | `make_bom.py` has no rotation offset table. U107 (SP809, KiCad stock `SOT-23`) and U108 (OPA197, KiCad stock `SOT-23-5`) have pad 1 at (-0.94, -0.95) mm and (-1.14, -0.95) mm from the origin at 0 deg. The EasyEDA-derived SOT-23 and SOT-23-5 footprints on the same board (Q101, D107, U4) have pad 1 at (+1.00, +0.95) mm and (+1.30, +0.95) mm at 0 deg. The two families differ by 180 deg. If JLC's zero is its own library orientation, U107 and U108 reach the board turned 180 deg, which swaps the supply and ground pins. Fix: add `{"SOT-23": 180, "SOT-23-5": 180}` to a footprint-keyed offset table applied in `write_cpl`, then confirm both parts on the JLC placement preview. |
| F4 | FIX | Pin-1 silk marks that are only a 0.12 mm dot, below the 0.153 mm JLC silk minimum, so they may not print: U3, U4, Q101, Q102, D107, D108. U104, U105, U106 have the same outside dot; their 0.60 mm dots sit under the body. Fix: replace each dot with a filled 0.30 mm circle beside pad 1, as already done on U6, U101, D2. |
| F5 | FIX | `hardware/PARTS.md` is stale against the board (section 3): 17 LCSC numbers on the board are missing, 3 rows are no longer on the board, and K103, D110, Q102 and the new 1 k and 100 k resistors are not counted. |
| O10 | OK | Assembly scope (corrected brief): the 27 through-hole panel parts (RV101..RV116, RVGAIN1, SW101..SW108, SWBYPASS1, ENC101) and J117 are in the BOM and CPL with valid LCSC numbers and stock. J101, J106, J108 and SWPWR1 (all B.Cu) are absent from both files. |
| D1 | DECIDE | C19 (tantalum C8649, KiCad stock `CP_EIA-6032-28_Kemet-C`) and J117 (hand-written OLED socket footprint) have no verifiable zero orientation against JLC. C19 pad 1 (+) is at (-2.47, 0) mm at 0 deg. J117 pad 1 is at (-3.81, -1.80) mm. Recommendation: check both on the JLC placement preview; add an offset only if the preview shows them turned. |
| D2 | DECIDE | Eleven parts moved between v2 and v3 without being on the expected-change list: C8, C13, C18, C45, C112, C113, C116, C132, D106, L101, R105 (section 2). Largest move 0.49 mm (C112), no rotation changes, all from commit 560d966. Recommendation: accept, no action. |
| O1 | OK | All 73 BOM lines match LCSC package to footprint (section 1). |
| O2 | OK | Every in-BOM, non-DNP footprint has a non-empty LCSC field; schematic and board agree on value, footprint and LCSC for all 242 shared references. |
| O3 | OK | Stock covers the build on every line; lowest is the pot C361173 at 135 pieces for 17 needed. |
| O4 | OK | The v2 to v3 BOM and CPL changes match the expected list (section 2). |
| O5 | OK | No duplicate references and no empty values. Every BOM part is on F.Cu; the only B.Cu footprints are J101, J106, J108 and SWPWR1. |
| O6 | OK | Pin names of 18 parts from `jlc_get_pinout` match the netlist pad functions (section 4). |
| O7 | OK | Gerber origin and CPL origin agree: the board has no aux axis origin, and the v2 gerbers are plotted with `SameCoordinates Original`. |
| O8 | OK | Polarity and pin-1 marks are visible after assembly for U1, U6, U8, U101, U103, D1, D2, D3, D106, D109, D110, C19, the eight polarised electrolytics (chamfer on the plus side) and K102, K103 (section 4). |
| O9 | OK | C412495 (SP809) is listed as open-drain by JLC, push-pull per PARTS.md. R158 (100 kohm to 3V3) pulls LDO_EN up, so U4 EN is valid with either output type. |

## 1. Generated BOM against LCSC (all 73 lines)

Tier counts: 42 basic, 31 extended. Verdict is OK where the JLC package matches the footprint size and stock is at least ten times the quantity used. Stock is the `jlc_get_part` figure on 2026-10-10.

| ref(s) | value | footprint | LCSC | JLC package | stock | tier | verdict |
|---|---|---|---|---|---|---|---|
| C2..C163 (34) | 100nF | C0603 / C_0603_1608Metric | C14663 | 0603 | 56537637 | basic | OK |
| C6 | 4.7uF | C_0603_1608Metric | C19666 | 0603 | 2210949 | basic | OK |
| C8,C10,C24 | 1uF | C_0603_1608Metric | C15849 | 0603 | 8651827 | basic | OK |
| C12..C111 (6) | 2.2uF | C0603 / C_0603_1608Metric | C23630 | 0603 | 2269120 | basic | OK |
| C15,C16 | 15pF | C_0603_1608Metric | C1644 | 0603 | 963616 | basic | OK |
| C18 | 1uF | C_0603_1608Metric | C90540 | 0603 | 830593 | extended | OK |
| C19 | 100uF 10V | CP_EIA-6032-28_Kemet-C | C8649 | CASE-C 6032-28 | 322411 | extended | OK - tantalum, KiCad stock footprint, see D1 |
| C20..C115 (7) | 22uF | C1206 / C_1206_3216Metric | C12891 | 1206 | 653587 | basic | OK |
| C28..C48 (5) | 10uF | C_0603_1608Metric | C19702 | 0603 | 10419984 | basic | OK |
| C55 | 1nF | C_0603_1608Metric | C1588 | 0603 | 7127051 | basic | OK |
| C56,C58 | 10nF | C_0402_1005Metric | C15195 | 0402 | 3598539 | basic | OK |
| C57..C167 (5) | 100nF | C_0402_1005Metric | C1525 | 0402 | 24133848 | basic | OK |
| C117,C135,C137,C138 | 10uF | C1206 | C14860 | 1206 | 879413 | extended | OK |
| C119,C123 | 100pF | C0603 | C14858 | 0603 | 3525114 | basic | OK |
| C120,C124 | 47uF 25V | CAP-SMD_BD6.3-L6.6-W6.6-LS7.6-FD | C72521 | D6.3 x L5.4 mm | 62127 | extended | OK |
| C121..C160 (5) | 10uF 25V | CAP-SMD_BD4.0-L4.3-W4.3-FD | C72484 | D4 x L5.4 mm | 103080 | extended | OK |
| C128,C129,C132 | 47pF | C0603 | C1671 | 0603 | 577513 | basic | OK |
| C136,C161 | 2.2nF | C0603 | C2991172 | 0603 | 34563 | extended | OK |
| C142 | 10nF | C0603 | C57112 | 0603 | 6146614 | basic | OK |
| C158 | 22uF 25V NP | CAP-SMD_BD6.3-L6.6-W6.6-FD | C413679 | D6.3 x L5.8 mm | 655 | extended | OK - non-polarised |
| C159 | 100uF 10V | CAP-SMD_BD5.0-L5.3-W5.3-LS6.3-FD | C191859 | D5 x L5.4 mm | 157228 | extended | OK |
| C164,C168,C169,C170 | 10uF | C_0805_2012Metric | C15850 | 0805 | 4326938 | basic | OK |
| D1,D3 | USER red / PWR red | LED-SMD_L1.6-W0.8-R-RD | C2286 | 0603 LED | 7292564 | basic | OK (L1.6 x W0.8 mm is 0603) |
| D2 | USBLC6-2SC6 | SOT-23-6_L2.9-W1.6-P0.95-LS2.8-BL | C2687116 | SOT-23-6 | 69635 | extended | OK |
| D105 | SMBJ20CA | SMB_L4.4-W3.6-LS5.4-BI | C113992 | SMB (DO-214AA) | 37190 | extended | OK |
| D106 | SS34 | SMA_L4.3-W2.6-LS5.2-RD | C8678 | SMA (DO-214AC) | 4167371 | basic | OK |
| D107,D108 | BAT54S | SOT-23_L2.9-W1.3-P1.90-LS2.4-BR | C19726 | SOT-23 | 163060 | extended | OK |
| D109,D110 | 1N4148W | SOD-123F_L2.7-W1.6-LS3.8-RD | C81598 | SOD-123 | 4949988 | basic | OK - footprint is SOD-123F, same land pattern family |
| ENC101 | EC11E15244B2 | SW-TH_EC11E | C470754 | THT EC11 | 5423 | extended | OK - through-hole |
| FB1,FB101,FB102 | 120R@100MHz | L_0603_1608Metric | C14709 | 0603 | 1487486 | basic | OK |
| J1 | USB-C | USB-C-SMD_TYPE-C16PIN | C393939 | SMD USB-C 16P | 372928 | extended | OK |
| J117 | OLED SH1106 | OLED-1.3-SH1106-I2C-4P | C42379197 | SMD 1x4 socket | 17288 | extended | OK |
| K102,K103 | G6K-2F-Y DC5 | RELAY-SMD_G6K-2F-X-XX | C47190 | SMD 10 x 6.5 mm | 64551 | extended | OK |
| L1 | 3.9uH | IND-SMD_L5.0-W5.0 | C7427132 | SMD 5 x 5 mm | 1187 | extended | OK |
| L101 | 4.7uH | IND-SMD_L5.0-W5.0_ANR5040T | C7427135 | SMD 5 x 5 mm | 17659 | extended | OK |
| Q101,Q102 | MMBT3904 | SOT-23-3_L2.9-W1.3-P1.90-LS2.4-BR | C20526 | SOT-23 | 331891 | basic | OK |
| R1 | 0R | R_0603_1608Metric | C21189 | 0603 | 25228968 | basic | OK |
| R2..R152 (19) | 10k | R0603 / R_0603_1608Metric | C25804 | 0603 | 26108467 | basic | OK |
| R3,R10 | 680R | R_0603_1608Metric | C23228 | 0603 | 2322057 | basic | OK |
| R4,R5 | 5.1k | R_0603_1608Metric | C23186 | 0603 | 24611641 | basic | OK |
| R6 | 33k | R_0603_1608Metric | C4216 | 0603 | 3797633 | basic | OK |
| R7 | 68k | R_0603_1608Metric | C23231 | 0603 | 749989 | basic | OK |
| R20 | 10R | R_0603_1608Metric | C22859 | 0603 | 6814936 | basic | OK |
| R27,R156,R157 | 33R | R_0402_1005Metric | C25105 | 0402 | 1685687 | basic | OK |
| R29 | 33R | R_0603_1608Metric | C23140 | 0603 | 5336235 | basic | OK |
| R30,R119,R124 | 100R | R0603 / R_0603_1608Metric | C22775 | 0603 | 10460618 | basic | OK |
| R101,R104 | 47R | R0603 | C23182 | 0603 | 1120670 | basic | OK |
| R102,R105 | 22k | R0603 | C31850 | 0603 | 3573520 | basic | OK |
| R103,R106 | 1.2k | R0603 | C22765 | 0603 | 2607928 | basic | OK |
| R108,R110 | 4.53k | R0603 | C25971 | 0603 | 14559 | extended | OK |
| R109..R160 (5) | 1k | R0603 | C21190 | 0603 | 19272175 | basic | OK |
| R111,R112,R113,R114 | 10k 0.1% | R0603 | C309083 | 0603 | 456381 | extended | OK |
| R116,R147 | 470R | R0603 | C23179 | 0603 | 6031033 | basic | OK |
| R123 | 15k | R0603 | C22809 | 0603 | 4343890 | basic | OK |
| R125..R161 (7) | 100k | R0603 | C25803 | 0603 | 21003892 | basic | OK |
| R142,R143 | 4.7k | R0603 | C23162 | 0603 | 20987885 | basic | OK |
| R146 | 22R | R0603 | C23345 | 0603 | 8408202 | basic | OK |
| R154,R155 | 10k | R_0402_1005Metric | C25744 | 0402 | 19810438 | basic | OK |
| R158 | 100k | R_0402_1005Metric | C25741 | 0402 | 8094761 | basic | OK |
| RV101..RVGAIN1 (17) | 10k / 10k GAIN | RES-ADJ-TH_RK09D1130C2P | C361173 | THT pot | 135 | extended | OK - through-hole, 17 needed, min order 15 |
| SW101..SWBYPASS1 (9) | ST-0-102 bat toggle | SW-TH_3P-L8.3-W5.2-P2.54_ST-0-102-A01-T000 | C1788487 | THT toggle | 5362 | extended | OK - through-hole |
| SWBOOT1,SWRESET1 | BOOT0 / RESET | SW-SMD_4P-L5.1-W5.1-P3.70-LS6.5-TL_H1.5 | C318884 | SMD 5.1 x 5.1 mm | 382343 | basic | OK |
| U1 | STM32H743VIT6 | LQFP-100_L14.0-W14.0-P0.50-LS16.0-BL | C114409 | LQFP-100 | 2379 | extended | OK |
| U3 | AP63203WU-7 | TSOT-26_L2.9-W1.6-P0.95-LS2.8-BL | C780769 | TSOT-23-6 | 12789 | extended | OK |
| U4 | TLV75533PDBVR | SOT-23-5_L3.0-W1.7-P0.95-LS2.8-BR | C404027 | SOT-23-5 | 133406 | extended | OK |
| U6 | PCM5102APWR | TSSOP-20_L6.5-W4.4-P0.65-LS6.4-BL | C107671 | TSSOP-20 | 7671 | extended | OK |
| U8 | PCM1808PWR | TSSOP-14_L5.0-W4.4-P0.65-LS6.4-BL | C55513 | TSSOP-14 | 15563 | extended | OK |
| U101 | CD74HC4067 | SSOP-24_L8.1-W5.3-P0.65-LS7.8-BL | C98457 | SSOP-24 208 mil | 8461 | extended | OK |
| U103 | AP63205WU-7 | TSOT-23-6_L2.9-W1.6-P0.95-LS2.8-BL | C2071056 | TSOT-23-6 | 27500 | extended | OK |
| U104,U105,U106 | OPA2197IDR | SOIC-8_L4.9-W3.9-P1.27-LS6.0-BL | C139363 | SOIC-8 | 17421 | extended | OK |
| U107 | SP809EK-L-2-9/TR | SOT-23 | C412495 | SOT-23 (3-pin) | 16416 | extended | OK - JLC lists open-drain output, see O9 |
| U108 | OPA197IDBVR | SOT-23-5 | C221351 | SOT-23-5 | 83685 | extended | OK |
| Y1 | 25MHz 12pF | CRYSTAL-SMD_4P-L3.2-W2.5-BL | C9006 | SMD 3225-4P | 221174 | basic | OK |

## 2. Generated v3 against production v2

BOM, 73 lines against 75 in v2. CPL, 222 rows against 216.

| # | Ref(s) | Change | Expected |
|---|---|---|---|
| 1 | D110 | added to the C81598 1N4148W line; CPL (184.60, -82.50) mm, 90 deg | yes |
| 2 | K103 | added to the C47190 relay line; CPL (191.30, -83.00) mm, 0 deg | yes |
| 3 | Q102 | added to the C20526 MMBT3904 line; CPL (185.60, -75.50) mm, 180 deg | yes |
| 4 | R159, R160 | added to the 1 k line C21190; CPL (233.40, -74.75) mm and (185.35, -72.50) mm | yes |
| 5 | R161 | added to the 100 k line C25803; CPL (185.35, -70.50) mm | yes |
| 6 | R116 | 330 ohm C23138 became 470 ohm C23179 and joins R147; CPL moved from (234.50, -78.50) mm, 0 deg to (235.00, -74.75) mm, -90 deg | yes |
| 7 | RVGAIN1 | LCSC C470307 became C361173 and joins RV101..RV116; the v2 line "10k GAIN" is gone, comment now "10k / 10k GAIN" | yes |
| 8 | TP6..TP11 | not in BOM or CPL (in_bom no) | yes |
| 9 | C8 | CPL X 103.000 to 103.100 mm | no, see D2 |
| 10 | C13 | CPL Y -73.500 to -73.200 mm | no, see D2 |
| 11 | C18 | CPL Y -40.775 to -40.475 mm | no, see D2 |
| 12 | C45 | CPL Y -103.500 to -103.600 mm | no, see D2 |
| 13 | C112 | CPL Y -53.000 to -52.510 mm | no, see D2 |
| 14 | C113 | CPL Y -53.000 to -53.400 mm | no, see D2 |
| 15 | C116 | CPL X 93.500 to 93.300 mm | no, see D2 |
| 16 | C132 | CPL X 231.070 to 231.000 mm | no, see D2 |
| 17 | D106 | CPL Y -49.000 to -49.250 mm | no, see D2 |
| 18 | L101 | CPL X 96.000 to 95.850 mm | no, see D2 |
| 19 | R105 | CPL Y -79.500 to -79.490 mm | no, see D2 |

Removed lines: 330R C23138 (R116) and the separate 10k GAIN C470307 line. The 470R line now holds R116 and R147. No part was removed from the CPL.

Command: `diff /tmp/bomaudit/bom.csv hardware/production/cubevox-bom-v2.csv` and the same for the CPL.

## 3. Board against `hardware/PARTS.md`

On the board, missing from PARTS.md (17 LCSC numbers):

| LCSC | Value | Refs |
|---|---|---|
| C19666 | 4.7 uF 16 V X5R 0603 | C6 |
| C15849 | 1 uF 50 V X5R 0603 | C8, C10, C24 |
| C23630 | 2.2 uF 16 V X5R 0603 | C12, C13, C25, C34, C35, C111 |
| C1644 | 15 pF C0G 0603 | C15, C16 |
| C8649 | 100 uF 10 V tantalum 6032 | C19 |
| C19702 | 10 uF 10 V X5R 0603 | C28, C30, C32, C46, C48 |
| C1588 | 1 nF X7R 0603 | C55 |
| C15195 | 10 nF X7R 0402 | C56, C58 |
| C1525 | 100 nF 16 V X7R 0402 | C57, C61, C165, C166, C167 |
| C2286 | red LED 0603 | D1, D3 |
| C14709 | ferrite 120 ohm 0603 | FB1, FB101, FB102 |
| C21189 | 0 ohm 0603 | R1 |
| C23228 | 680 ohm 0603 | R3, R10 |
| C23186 | 5.1 k 0603 | R4, R5 |
| C22859 | 10 ohm 0603 | R20 |
| C25105 | 33 ohm 0402 | R27, R156, R157 |
| C25744 | 10 k 0402 | R154, R155 |

In PARTS.md, no longer on the board:

| Row | LCSC | Status |
|---|---|---|
| 20c | C470307 RK09D117000C | RVGAIN1 now uses C361173; the row's "30 mm log taper" text is out of date |
| 40 | C25810 18 k | no 18 k resistor on the board |
| 68 | C23138 330 ohm | R116 is now 470 ohm C23179 |

Rows 19, 20b and 29 are already marked superseded and are fine. Rows for C368458, C309282, C3096082 and C17179528 (the four owner-soldered parts) are correctly absent from the BOM.

Quantity and text drift in rows that still match by LCSC:

1. Row 13 relay: lists K102 only, board has K102 and K103 (qty 2).
2. Row 32 1N4148W: lists D109 only, board has D109 and D110.
3. Row 63 MMBT3904: lists Q101 only, board has Q101 and Q102.
4. Row 20 pots: 16 on C361173 plus RVGAIN1 makes 17 on that number.
5. Row 55 470 ohm: lists R147 only, board has R116 and R147.
6. Counts item 1 says 61 unique part numbers; the BOM has 73. Counts item 4 lists the 10 ohm resistor, the 5.1 k resistors and the BLM18PG121SN1D ferrite as "not yet coded"; all three are on the board.

## 4. Polarity and pin 1

Pad 1 position is the netlist pad function and the KiCad pad coordinate. LCSC pin 1 is the `jlc_get_pinout` name. "Mark" is the F.Silkscreen feature beside pad 1 and whether it clears the part body (outside body means visible after assembly). F.Fab carries no pin-1 mark on any of these parts and is not visible after assembly.

| Ref | Part | Pad 1 (x, y) mm | Pad 1 net | LCSC pin 1 | Silk mark | Verdict |
|---|---|---|---|---|---|---|
| U1 | STM32H743VIT6 | 90.00, 91.55 | PE2 | PE2 | 0.60 mm arc at corner, outside | OK; sampled pads 1, 2, 25, 26, 50, 51, 75, 76, 100 match |
| U3 | AP63203 | 96.45, 59.80 | FB | FB | 0.12 mm dot only | pin OK, mark F4 |
| U103 | AP63205 | 93.05, 54.10 | FB | FB | 0.51 mm dot, outside | OK |
| U4 | TLV75533 | 152.80, 49.45 | IN | IN | 0.12 mm dot only | pin OK, mark F4 |
| U6 | PCM5102A | 75.53, 103.37 | CPVDD | CPVDD | 0.60 mm dot, outside | OK; all 20 pad functions match |
| U8 | PCM1808 | 126.80, 104.45 | VREF | VREF | outline notch 0.25 mm, 0.12 mm dot | OK; all 14 pad functions match |
| U101 | CD74HC4067 | 151.56, 131.08 | COM | COM | 0.60 mm dot, outside | OK; all 24 pad functions match |
| U104 | OPA2197 | 207.60, 81.40 | OUT/A | OUT/A | 0.12 mm dot outside, 0.60 mm dot under body | pin OK, mark F4 |
| U105 | OPA2197 | 229.10, 87.10 | OUT/A | OUT/A | same | pin OK, mark F4 |
| U106 | OPA2197 | 72.40, 87.80 | OUT/A | OUT/A | same | pin OK, mark F4 |
| U107 | SP809 | 158.70, 47.55 | GND | GND | 0.45 x 0.60 mm triangle beside pad 1 | pin OK, rotation F3 |
| U108 | OPA197 | 159.61, 88.55 | OUT | OUT | 0.60 x 0.45 mm triangle beside pad 1 | pin OK, rotation F3 |
| D1 | USBLC6-2SC6 | 113.85, 40.65 | I/O1 | I/O1 | 0.60 mm dot, outside | OK |
| D107, D108 | BAT54S | 211.05, 64.65 and 197.05, 64.65 | A (to GND) | A | 0.12 mm dot only | pin OK (A, C, A/C match), mark F4 |
| Q101, Q102 | MMBT3904 | 72.50, 78.45 and 184.60, 74.55 | B | B | 0.12 mm dot only | pin OK (B, E, C match), mark F4 |
| D109, D110 | 1N4148W | 71.20, 72.33 and 184.60, 84.23 | K to 5V | n/a | 0.3 mm triangles at the cathode end, outside | OK; flyback polarity correct (K on 5V, A on coil node) |
| D106 | SS34 | 234.35, 51.45 | K to VIN_9V | n/a | 0.40 x 0.46 mm triangles at the cathode end, outside | OK; K on VIN_9V, A on 9V_JACK |
| D105 | SMBJ20CA | 229.59, 51.45 | VIN_9V | bidirectional | dot only | OK, no polarity |
| D1, D3 | red LED | 82.93, 86.50 and 108.30, 75.32 | anode | A | 0.20 mm dot, outside | OK; the dot marks the anode, not the cathode |
| K102 | G6K-2F-Y | 74.20, 74.01 | 5V (coil +) | 1 | 0.20 mm dot at the body corner, coil symbol | OK; relay body carries its own pin-1 mark |
| K103 | G6K-2F-Y | 187.50, 86.51 | 5V (coil +) | 1 | same | OK |
| C19 | tantalum 100 uF | 106.53, 46.00 | 5V (+) | n/a | bar at the pad 1 edge, outside the body | OK, rotation D1 |
| C120, C124 | 47 uF electrolytic | 212.00, 73.17 and 198.00, 73.17 | IN_P and IN_N (+) | n/a | chamfered base corners on the pad 1 side | OK |
| C121, C125 | 10 uF electrolytic | 218.50, 72.16 and 191.50, 72.16 | LINE_P and LINE_N (+) | n/a | chamfer on pad 1 side | OK |
| C139, C141, C160 | 10 uF electrolytic | 65.75, 101.04; 79.60, 80.66; 155.84, 97.00 | DAC_AC; BUF_R; VREF (+) | n/a | chamfer on pad 1 side | OK |
| C159 | 100 uF electrolytic | 237.00, 81.20 | G2_RG (+) | n/a | chamfer on pad 1 side | OK |
| C158 | 22 uF bipolar | 208.17, 90.00 | A1_FB | non-polar | chamfer shown, no polarity needed | OK |
| Y1 | 25 MHz crystal | 95.35, 97.60 | OSC1 | OSC1 | 0.12 mm dot only | pin OK; pin 1 and 3 are the crystal terminals, so a 180 deg turn is harmless |
| J1 | USB-C | pad A1B12 118.20, 36.16 | GND | n/a | no pin-1 meaning | OK |
| J117 | OLED socket | 131.97, 55.95 | GND | 1 | none on silk; F.Fab label "1" | pin OK; JLC places by CPL, rotation D1 |

Relay pin map: coil 1 positive and 8 negative, commons 3 and 6, NC 2 and 7, NO 4 and 5. This comes from the 2026-10-02 schematic audit line 28. The 2026-10-06 round 6 audit says the G6K map was "not verified from a datasheet", and the Omron PDF at the JLC link returned no diagram when fetched today. `jlc_get_pinout` gives only the numbers 1 to 8. The board nets agree with the 2026-10-02 map for both relays (K102: 3 to OUT_AC, 4 and 6 to OUT_TIP, 7 to GND; K103: 3 to LINE_P, 4 to IN_P, 5 to LINE_N, 6 to IN_N). A bench continuity check on the first board is still needed.

## 5. Rotation

`make_bom.py` writes the raw KiCad rotation. The table shows pad 1 at 0 deg for every non-symmetric BOM footprint. Footprints named like easyeda2kicad output (`...-BL`, `...-BR`, `...-LS...`) are the EasyEDA library footprint for that LCSC number, so their zero orientation is JLC's by construction and needs no offset. KiCad stock footprints and hand-written ones are the risk. `jlc_get_part` does not return JLC's zero orientation, so the stock footprints below are judged against the EasyEDA footprints of the same package on this board.

| Footprint | Refs | Pad 1 at 0 deg (x, y) mm | Origin | Offset in script | Verdict |
|---|---|---|---|---|---|
| SOT-23 (KiCad stock) | U107 | -0.94, -0.95 | stock | none | F3: 180 deg from the EasyEDA SOT-23 (1.15, 0.95) |
| SOT-23-5 (KiCad stock) | U108 | -1.14, -0.95 | stock | none | F3: 180 deg from the EasyEDA SOT-23-5 (1.30, 0.95) |
| CP_EIA-6032-28_Kemet-C (stock) | C19 | -2.47, 0.00 | stock | none | D2 |
| OLED-1.3-SH1106-I2C-4P (hand-written) | J117 | -3.81, -1.80 | cubevox | none | D2 |
| SOT-23-3 L2.9 ... BR | Q101, Q102 | 1.00, 0.95 | EasyEDA | none | OK |
| SOT-23 L2.9 ... BR | D107, D108 | 1.15, 0.95 | EasyEDA | none | OK |
| SOT-23-5 L3.0 ... BR | U4 | 1.30, 0.95 | EasyEDA | none | OK |
| SOT-23-6, TSOT-23-6, TSOT-26 | D2, U103, U3 | -0.95, 1.10 to 1.15 | EasyEDA | none | OK |
| SOIC-8, TSSOP-14, TSSOP-20, SSOP-24 | U104..U106, U8, U6, U101 | pad 1 at bottom-left | EasyEDA | none | OK |
| LQFP-100 | U1 | -6.00, 7.55 | EasyEDA | none | OK |
| RELAY-SMD_G6K | K102, K103 | -3.80, 3.51 | EasyEDA | none | OK |
| SW-SMD_4P tact | SWBOOT1, SWRESET1 | -3.00, -1.85 | EasyEDA | none | OK |
| CRYSTAL 3.2 x 2.5 | Y1 | -1.10, 0.85 | EasyEDA | none | OK |
| LED, SOD-123F, SMA, SMB, IND, CAP-SMD_BD, USB-C | D1, D3, D109, D110, D106, D105, L1, L101, C120.., J1 | see section 4 | EasyEDA | none | OK |
| R0603, C0603, C1206, C_0402, C_0603, C_0805, C_1206, R_0402, L_0603 | passives | symmetric | cubevox or stock | none | OK, a 180 deg turn changes nothing |

Parts with no offset table entry: all of them, because the table does not exist (F3). The offset is needed only for the two stock SOT-23 footprints.

Origin check: for every BOM footprint the pad bounding box centre sits within 0.02 mm of the footprint origin, so the CPL centre equals the part centre for JLC. The largest offset is J1 at 0.02 mm.

## 6. Duplicates and gaps

| Check | Result |
|---|---|
| Schematic components (netlist) | 242 |
| Board footprints | 245 |
| In schematic only | none |
| On board only | FID1, FID2, FID3 (fiducials, no symbol, not in BOM) |
| Duplicate references on the board | none |
| Footprints with an empty value | none |
| Value, footprint or LCSC differing between schematic and board | none on 242 shared references |
| In BOM yes and DNP no, empty LCSC | none |
| Excluded from BOM, no LCSC | H101..H104, J3, TP1..TP11 (matches the brief) |
| Excluded on the board only | J101, J106, J108; the schematic does not flag them, `OWNER_SOLDERED` in the script covers it |
| Excluded in both | SWPWR1 |
| DNP footprints | none |

Commands: `kicad-cli sch export netlist`, then `make_bom.parse_sexpr` compared with `board.json` from `pcbnew.LoadBoard` (scratch scripts in `/tmp/bomaudit/`).

## Summary

| Class | Count |
|---|---|
| FIX | 5 (F1 to F5) |
| DECIDE | 2 (D1, D2) |
| OK | 10 (O1 to O10) |
