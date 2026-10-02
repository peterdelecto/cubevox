# cubevox PARTS

Board: cubevox main board (one JLC-assembled board). Spec: docs/specs/2026-10-02-hardware-design.md.
Snapshot 2026-10-02. Prices are the 10+ tier. Stock is from `jlc_get_part` unless marked LIVE
(`jlc_stock_check`, 2026-10-02). Re-check everything at order time.
Build quantity: 1 board (qty column is per board; required = per-board x N + allowance, 15 pots x N).

## Parts

| # | LCSC | MPN | Role | Tier | Stock | $ @10+ | Footprint (lib) | Qty/bd | Import verdict |
|---|------|-----|------|------|-------|--------|-----------------|--------|----------------|
| 1 | C114409 | STM32H743VIT6 | MCU | Extended | 3681 | 10.69 | LQFP-100 (h7core) | 1 | inherited, in lib |
| 2 | C9006 | X322525MOB4SI | 25 MHz crystal | Basic | 199401 | 0.076 | CRYSTAL-SMD_4P 3.2x2.5 (h7core) | 1 | inherited |
| 3 | C393939 | TYPE-C16PIN | USB-C | Extended | 397977 | 0.055 | USB-C-SMD_TYPE-C16PIN (h7core) | 1 | inherited |
| 4 | C2687116 | USBLC6-2SC6 | USB ESD | Extended | 82399 | 0.038 | SOT-23-6 (h7core) | 1 | inherited |
| 5 | C3235557 | TPS2116DRLR | USB/9V 5 V power mux | Extended | 79385 | 0.306 | SOT-583-8 (h7core) | 1 | inherited |
| 6 | C2071056 | AP63205WU-7 | 5 V buck | Extended | 36399 | 0.319 | TSOT-23-6 (cubevox) | 1 | inherited |
| 7 | C7427135 | ANR5040T4R7M | AP63205 inductor 4.7 uH | Extended | 17676 | 0.043 | IND-SMD 5x5 (cubevox) | 1 | inherited |
| 8 | C780769 | AP63203WU-7 | 3V3 buck | Extended | 17725 | 1.054 | TSOT-26 (h7core) | 1 | inherited |
| 9 | C7427132 | ANR5040T3R9M | AP63203 inductor 3.9 uH | Extended | 1235 | 0.050 | IND-SMD 5x5 (h7core) | 1 | inherited |
| 10 | C404027 | TLV75533PDBVR | 3V3A LDO | Extended | 136560 | 0.113 | SOT-23-5 (h7core) | 1 | inherited |
| 11 | C55513 | PCM1808PWR | ADC | Extended | 27700 | 0.563 | TSSOP-14 (h7core) | 1 | inherited |
| 12 | C107671 | PCM5102APWR | DAC | Extended | 6661 | 1.276 | TSSOP-20 (h7core) | 1 | inherited |
| 13 | C47190 | G6K-2F-Y-TR DC5 | bypass relay DPDT 5 V | Extended | 68587 | 0.915 | RELAY-SMD_G6K-2F (cubevox) | 1 | inherited |
| 14 | C3096082 | PJ-002A | 9 V barrel jack | Extended | 1893 | 0.753 | DC-IN-TH_PJ-002A (cubevox) | 1 | inherited |
| 16 | C19077570 | SMBJ15CA | 9 V TVS | Preferred | 150573 | 0.044 | SMB (cubevox) | 1 | inherited |
| 17 | C98457 | CD74HC4067SM96 | 16:1 pot mux | Extended | 9499 | 0.460 | SSOP-24 (cubevox) | 1 | inherited |
| 18 | C470748 | EC11N1525404 | MENU encoder, with switch | Extended | 984 | 2.375 | SW-TH_EC11NXXXX (cubevox) | 1 | inherited; confirmed in lib |
| 19 | C470703 | EC11N1520401 | KEY, SEMITONES encoders, no switch | Extended | 977 (LIVE) | 1.818 | SW-TH_EC11NXXXX (cubevox) | 2 | IMPORTED, courtyard vs model clean, 7 pads (6 contacts) |
| 20 | C361173 | RK09D1130C2P | pots (superseded, do not order) | Extended | 174 (LIVE) | 0.629 | RES-TH_RK09D1130C2P (cubevox) | 0 | SUPERSEDED by C470304, do not order. Alps F25 = 25 mm shaft; owner measured 13.5 mm on their sample; both are wrong for the 19.5 mm encoder tip |
| 20b | C470304 | RK09D1130C1B | pots, 14 panel + Input Gain | Extended | 986 (LIVE 2026-10-02) | 0.84 (10+) | RES-ADJ-TH_RK09D1130C3C (cubevox) | 15 | IMPORTED with --repair-courtyard; model: Alps STEP aligned (RK09D1130C1B_alps_aligned.step), shaft tip 20.0 measured |
| 21 | C368458 | NCJ6FA-H | XLR + 1/4" combo input | Extended | 111 (LIVE) | 2.600 | CONN-TH_NCJ6FA-H (cubevox) | 1 | IMPORTED with --model-offset -1.24 -11.46 0 and --accept-mismatch (catalogue 3P is the XLR pin count), see WARNINGS 1 |
| 22 | C368502 | NMJ6HCD2 | 1/4" output jack | Extended | 1358 (LIVE) | 2.917 | AUDIO-TH_NMJ6HCD2 (cubevox) | 1 | IMPORTED with --repair-courtyard |
| 24 | C19726 | BAT54SLT1G | preamp input clamps | Extended | 256249 | 0.019 | SOT-23 (h7core) | 2 (est.) | inherited |
| 25 | C318884 | TS-1187A-B-A-B | BOOT0, NRST tact | Basic | 473784 | 0.016 | SW-SMD_4P 5.1x5.1 (h7core) | 2 (est.) | inherited |
| 26 | C2841348 | IS25LP064A-JBLE-TR | QSPI flash | Extended | 2146 | 2.068 | SOIC-8-208 (h7core) | 1 | inherited |
| 28 | C2691448 | PZ254V-11-04P | OLED header and SWD header, 1x4 2.54 mm | Extended | 979885 | 0.021 | HDR-TH_4P-P2.54-V-M (cubevox) | 2 | IMPORTED, clean |
| 29 | C192421 | OPA1678IDR | A1-A5 + VREF buffer (3 duals) | Extended | 3309 | 0.774 | SOIC-8_L5.0-W4.0 (cubevox) | 3 | IMPORTED with --repair-courtyard |
| 30 | C8678 | SS34 | 9V_JACK to VIN_9V Schottky | Basic | 3557042 | 0.030 | SMA_L4.3-W2.6 (cubevox) | 1 | IMPORTED with --repair-courtyard |
| 31 | C2937625 | PZ254V-11-03P | 1x3 header, unused since the toggle part was chosen | Extended | 1200838 | 0.020 | HDR-TH_3P-P2.54-V-M (cubevox) | 0 | IMPORTED with --repair-courtyard |
| 32 | C81598 | 1N4148W | relay flyback | Basic | 5171770 | 0.012 | SOD-123F (cubevox) | 1 | inherited |
| 32a | C1788487 | ST-0-102-A01-T000-LF | toggle switches, SPDT ON-ON two-position (datasheet p.2 function table: model 102 = POS.1 ON, POS.2 NONE, POS.3 ON), 3 pins 2.54 mm; 8 panel + bypass | Extended | 5513 (LIVE 2026-10-02) | 0.538 (10+) | SW-TH_3P-L8.3-W5.2-P2.54_ST-0-102-A01-T000 (cubevox) | 9 | IMPORTED with --accept-mismatch; courtyard edited in .kicad_mod, see WARNINGS 11. Pins: 2 = COM, 1 = T1, 3 = T3 |
| 33 | C23182 | 0603WAF470JT5E | R 47 ohm | Basic | 975974 | 0.002 | R0603 (cubevox) | by schematic | IMPORTED, courtyard repaired |
| 34 | C22775 | 0603WAF1000T5E | R 100 ohm | Basic | 7325193 | 0.002 | R0603 | by schematic | IMPORTED |
| 35 | C21190 | 0603WAF1001T5E | R 1 k | Basic | 8013731 | 0.004 | R0603 | by schematic | IMPORTED |
| 36 | C22765 | 0603WAF1201T5E | R 1.2 k | Basic | 901591 | 0.002 | R0603 | by schematic | IMPORTED |
| 37 | C23162 | 0603WAF4701T5E | R 4.7 k | Basic | 7433362 | 0.002 | R0603 | by schematic | IMPORTED |
| 38 | C25804 | 0603WAF1002T5E | R 10 k | Basic | 23056985 (LIVE; local catalog says 0, stale) | 0.002 | R0603 | by schematic | IMPORTED |
| 39 | C22809 | 0603WAF1502T5E | R 15 k | Basic | 1150042 | 0.002 | R0603 | by schematic | IMPORTED |
| 40 | C25810 | 0603WAF1802T5E | R 18 k | Basic | 410427 | 0.002 | R0603 | by schematic | IMPORTED |
| 41 | C23140 | 0603WAF330JT5E | R 33 ohm | Basic | 1772784 | 0.001 | R0603 | by schematic | IMPORTED |
| 42 | C25803 | 0603WAF1003T5E | R 100 k | Basic | 7990119 | 0.002 | R0603 | by schematic | IMPORTED |
| 43 | C1671 | CL10C470JB8NNNC | C 47 pF C0G 50 V | Basic | 360883 | 0.006 | C0603 (cubevox) | by schematic | IMPORTED |
| 44 | C14858 | CL10C101JB8NNNC | C 100 pF C0G 50 V | Basic | 1658528 | 0.007 | C0603 | by schematic | IMPORTED |
| 45 | C2991172 | 0603CG222J500NT | C 2.2 nF C0G 50 V (no Basic exists) | Extended | 80127 | 0.030 | C0603 | by schematic | IMPORTED |
| 46 | C14663 | CC0603KRX7R9BB104 | C 100 nF X7R 50 V | Basic | 12618106 | 0.011 | C0603 | by schematic | IMPORTED |
| 47 | C57112 | 0603B103K500NT | C 10 nF X7R 50 V | Basic | 2155057 | 0.020 | C0603 | by schematic | IMPORTED |
| 48 | C14860 | CL31B106KAHNNNE | C 10 uF X7R 25 V 1206 (DC blocks, VREF; no Basic X7R exists) | Extended | 917604 (LIVE) | 0.169 | C1206 (cubevox) | by schematic | IMPORTED |
| 49 | C12891 | CL31A226KAHNNNE | C 22 uF X5R 25 V 1206, 9V_IN | Basic | 574332 | 0.180 | C1206 | by schematic | IMPORTED; X5R loses capacitance under DC bias |

## Counts

1. Unique part numbers: 46 (19 Basic, 1 Preferred, 26 Extended). Row numbers are not contiguous (15, 23, 27 removed).
2. Fee estimate: 26 Extended x ~$3 = ~$78 per order. NCJ6FA-H is counted although it is not in the library yet.
3. Per-board quantities fixed: 3x OPA1678, 15 pots, 3 encoders, 9 bat toggles, 1 header 1x4, 1 relay, 1 SS34. Passive quantities come from the schematic.
4. Not yet coded (spec items): 10 ohm PCM1808 VCC resistor, 2.2 uF VCAP caps, CC 5.1 k C23186, BLM18PG121SN1D ferrite, the toggle switch itself (header placeholders only), clamp C27675.

## WARNINGS

1. C368458 NCJ6FA-H is imported. ClaudeRouter `pad_roles` now takes the card branch when `mechanical_pads` is present, even empty (uncommitted R change, awaiting owner review), so 8 numbered pads resolve as contacts. `pin_count_mismatch` (catalogue 3 vs 8) is accepted with the reason "catalogue 3P is the XLR pin count; footprint has 8 contacts per Neutrik ST-NCJ6FAH". `--model-offset -1.24 -11.46 0` clears the model overhang. Symbol pins are named 1, 2, 3, S, R, T, T_SW, G (pads 1-8), all passive. Which of pads 6 and 7 is T_SW is NOT VERIFIED. Stock 385 live 2026-10-02 pm. NCJ6FA-V C368485 is NOT a fallback (vertical mount, wrong angle; owner 2026-10-02). ASSEMBLY: J101 (combo), J108 (1/4" out) and J106 (barrel) sit on B.Cu and are hand-soldered by the owner; exclude them from JLC's BOM/CPL. USB-C J1 stays top, JLC-assembled.
2. Pad map from the Neutrik drawing ST-NCJ6FAH, matched by position (footprint x is mirrored against the component-side drawing): pad 1 = 1, 2 = 2, 3 = 3, 4 = S, 5 = R, 6 = T, 7 = T_SW (single reading), 8 = G. Which T is the normalling contact (T_SW) is NOT VERIFIED; the drawing does not say. Check by continuity before ordering.
3. Courtyards repaired with `--repair-courtyard` (vendor courtyard excluded the pads): NMJ6HCD2 26.61x18.29 -> 27.12x19.24 mm; OPA1678IDR 5.09x4.09 -> 5.60x7.88 mm; SS34 4.41x2.69 -> 6.90x3.20 mm; R0603 1.69x0.89 -> 2.82x1.40; C0603 1.69x0.89 -> 2.70x1.40; C1206 3.29x1.69 -> 5.18x2.24; HDR 1x3 repaired the same way.
4. SWD header C52016392 was refused (`no_courtyard`); C2691448 serves both OLED and SWD. No Basic 1x4 header exists on JLC. Spec item 14/16 note this.
5. AO4606 (C2944311) and SMBJ70CA (C224026) are dropped by the spec but still sit in cubevox.kicad_sym; remove with the schematic spec edit if wanted. cubevox.json still references PJ-611E and must be rewritten.
6. OPA1678IDR: pin types set by hand from the TI SOIC-8 pinout (1,7 output; 2,3,5,6 input; 4,8 power_in); symbol names/numbers matched the pinout; `assign_pin_types.py` now leaves 0 unspecified. Single-supply 5 V, rail-to-rail output; datasheet not read here.
7. Pin maps NOT VERIFIED: PJ-002A, NMJ6HCD2, OPA1678IDR symbol pin order. Axis candidates printed by the import are unverified.
8. Stocks: RK09D1130C1B 986 (15 per board), NCJ6FA-H 111. The local foreman catalogue shows C25804 at stock 0; live check shows 23,056,985 (catalogue stale).
9. Qty for BAT54S (2) and tact switches (2) are estimates until the schematic exists.
10. Pot and encoder shaft heights (spec item 17) are datasheet figures, not checked against the imported models. 22 uF is X5R (Basic); an X7R 22 uF would be Extended.
11. C1788487 ST-0-102-A01-T000-LF: import refused with model_courtyard_mismatch (left 1.30, top 2.85, right 2.60, bottom 2.85 mm). Cause: the .wrl holds the M5 hex nut, the dia 11 lock washer with its 12.3 mm tab, the lever (23.6 mm high) and the pins, so its plan extent is x -5.50..6.80, y +-5.49 mm against the 8.3 x 5.2 body courtyard. Body, pads (2.54 pitch) and model alignment match the datasheet; no offset or rotation was applied. Fix: imported with --accept-mismatch, then the four F.CrtYd lines were edited to x -5.75..7.05, y -5.75..5.75 (model plan + 0.25 mm). The silk outline (8.3 x 5.2) is unchanged. The footprint carries the washer tab on the +X side; the physical orientation of the tab is not set by the datasheet. Pin names COM/T1/T3 are mine (symbol was 1/2/3); pad 2 is the centre common. The symbol is stored as ST-0-102-A01-T000-LF+PJ.

## Dropped from fxbox

PJ-325M (C2884942), WS2812B-2020 (C52917434), B3F-4055 (C84931), RK09K pots (C470311, C209779), TPA6132A2 (C69901), AO4606 (C2944311), SMBJ70CA (C224026), NE5532DR (C7426), PJ-611E (C309282), unused probe headers C32713270 and C124378, TLV9062 (C398356), TPS63070 (C109322), LP5912 (C2761351).
