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
| 28 | C2691448 | PZ254V-11-04P | spare 1x4 header 2.54 mm (OLED moved to row 28b; no schematic part uses it now) | Extended | 979885 | 0.021 | HDR-TH_4P-P2.54-V-M (cubevox) | 0 | IMPORTED, clean |
| 28b | none (owner-supplied) | SH1106 1.3in I2C 128x64 | J117 OLED module, 4-pin I2C, hand-plugged by the owner, not on the JLC BOM | n/a | n/a | n/a | OLED-1.3-SH1106-I2C-4P (cubevox, hand-written) | 1 | Module PCB 35.4x33.5x1.2, pins GND/VCC/SCL/SDA, 4x NPTH 3.2 on a 30.4x28.5 grid. Buy 4x M3 x 8 mm standoffs (plus 4 screws). Model assumes 8.0 mm standoffs: module PCB bottom at z 8.0, glass top at z 10.7, pins down to z -3.0. The 11.8 mm panel plane needs about 9.1 mm standoffs. |
| 29 | C192421 | OPA1678IDR | SUPERSEDED by row 50 (revision 2, audit A02: input range stops 2 V below V+). Do not order | Extended | 3309 | 0.774 | SOIC-8_L5.0-W4.0 (cubevox) | 0 | footprint kept in cubevox.pretty, unused |
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
| 48 | C14860 | CL31B106KAHNNNE | C 10 uF X7R 25 V 1206. Revision 2: stays only at C137 (ADC feed), C138 (VINR), plus C117 (5VA bypass) and C135 (VREF divider), none a coupling position. Superseded as audio coupling cap by row 52 (audit A07) | Extended | 917604 (LIVE) | 0.169 | C1206 (cubevox) | 4 | IMPORTED |
| 49 | C12891 | CL31A226KAHNNNE | C 22 uF X5R 25 V 1206, 9V_IN | Basic | 574332 | 0.180 | C1206 | by schematic | IMPORTED; X5R loses capacitance under DC bias |
| 50 | C139363 | OPA2197IDR | A1-A4 (U104, U105), VREF follower + output buffer (U106): 3 duals | Extended | 9501 (import 2026-10-02; spec quotes 19.8 k live) | 0.8265 (import; spec quotes 1.26) | SOIC-8_L4.9-W3.9-P1.27-LS6.0-BL (cubevox) | 3 | IMPORTED with --repair-courtyard (4.99x3.99 -> 5.50x7.50 mm); model offset clean, no --accept-mismatch. Pin types by hand like OPA1678 (1,7 output; 2,3,5,6 input; 4,8 power_in); 0 unspecified |
| 51 | C309083 | ARG03BTC1002 | R111-R114, difference amp, 10 k 0.1 % 25 ppm | Extended | 59843 | 0.0214 | R0603 (cubevox) | 4 | IMPORTED with --repair-courtyard; same R0603 footprint as the other 0603 resistors |
| 52 | C72484 | RVT1E100M0405 | 10 uF 25 V electrolytic 4x5.4 (ROQANG, 105 C): C120, C121, C124, C125 coupling, C139 DAC, C141 output, C160 VREF node | Extended | 32285 | 0.0217 | CAP-SMD_BD4.0-L4.3-W4.3-FD (cubevox) | 7 | IMPORTED with --repair-courtyard (4.39x4.39 -> 6.62x4.90 mm). Symbol polarised, pin 1 = +. Footprint pad 1 = + (see WARNINGS 12) |
| 53 | C191859 | VT1A101M0505 | 100 uF 10 V electrolytic 5x5.4: C159 (stage 2 R116 leg) | Extended | 26521 | 0.0256 | CAP-SMD_BD5.0-L5.3-W5.3-LS6.3-FD (cubevox) | 1 | IMPORTED with --repair-courtyard (5.39x5.39 -> 8.00x5.90 mm). Pin 1 = + |
| 54 | C72502 | RVT1C220M0405 | 22 uF 16 V electrolytic 4x5.4: C158 (stage 1 RG leg) | Extended | 57055 | 0.0215 | CAP-SMD_BD4.0-L4.3-W4.3-LS5.3-FD (cubevox) | 1 | IMPORTED with --repair-courtyard (4.39x4.39 -> 7.00x4.90 mm). Pin 1 = + |
| 55 | C23179 | 0603WAF4700T5E | R 470 ohm: R147, DAC reconstruction filter | Basic | 1899614 | 0.0016 | R0603 | 1 | IMPORTED, courtyard repaired (same footprint) |
| 56 | C8218 | 0603WAF2000T5E | R 200 ohm: R116, stage 2 gain leg | Basic | 1461682 | 0.0016 | R0603 | 1 | IMPORTED, courtyard repaired |
| 57 | C23345 | 0603WAF220JT5E | R 22 ohm: R146, VREF follower isolation | Basic | 2657294 | 0.0017 | R0603 | 1 | IMPORTED, courtyard repaired |
| 58 | C23147 | 0603WAF3602T5E | R 36 k: R8, TPS2116 PR1 divider (replaces 33 k C4216, audit A06) | Extended (Preferred) | 303 | 0.0017 | R0603 | 1 | IMPORTED, courtyard repaired |
| 59 | C23127 | 0603WAF9531T5E | R 9.53 k: R108, R110, stage 1 feedback | Extended | 90 | 0.0019 | R0603 | 2 | IMPORTED, courtyard repaired. Stock 90 is thin; re-check at order time |

## Counts

1. Unique part numbers: 55 (22 Basic, 2 Preferred, 31 Extended). Row 29 (OPA1678) is superseded and not counted. Row numbers are not contiguous (15, 23, 27 removed).
2. Fee estimate: 31 Extended x ~$3 = ~$93 per order. NCJ6FA-H is counted although it is not in the library yet.
3. Per-board quantities fixed: 3x OPA2197, 15 pots, 3 encoders, 9 bat toggles, 1 header 1x4, 1 relay, 1 SS34. Passive quantities come from the schematic.
4. Not yet coded (spec items): 10 ohm PCM1808 VCC resistor, 2.2 uF VCAP caps, CC 5.1 k C23186, BLM18PG121SN1D ferrite, the toggle switch itself (header placeholders only), clamp C27675.

## Assembly

| Ref | Part | Assembled by |
|-----|------|--------------|
| J101 | NCJ6FA-H combo input (B.Cu) | owner, excluded from JLC BOM/CPL |
| J106 | PJ-002A barrel (B.Cu) | owner, excluded |
| J108 | NMJ6HCD2 1/4 in output (B.Cu) | owner, excluded |
| J117 | SH1106 OLED module and header | owner, excluded |
| all other refs | | JLC (top side, Economic) |

## WARNINGS

1. C368458 NCJ6FA-H is imported. ClaudeRouter `pad_roles` now takes the card branch when `mechanical_pads` is present, even empty (uncommitted R change, awaiting owner review), so 8 numbered pads resolve as contacts. `pin_count_mismatch` (catalogue 3 vs 8) is accepted with the reason "catalogue 3P is the XLR pin count; footprint has 8 contacts per Neutrik ST-NCJ6FAH". `--model-offset -1.24 -11.46 0` clears the model overhang. Symbol pins are named 1, 2, 3, S, R, T, T_SW, G (pads 1-8), all passive. Revision 2: pads 6 and 7 are both the tip (Neutrik electrical diagram, audit A01); both go to TRS_T and there is no plug detection. Stock 385 live 2026-10-02 pm. NCJ6FA-V C368485 is NOT a fallback (vertical mount, wrong angle; owner 2026-10-02). ASSEMBLY: J101 (combo), J108 (1/4" out) and J106 (barrel) sit on B.Cu and are hand-soldered by the owner; exclude them from JLC's BOM/CPL. USB-C J1 stays top, JLC-assembled.
2. Pad map from the Neutrik drawing ST-NCJ6FAH, matched by position (footprint x is mirrored against the component-side drawing): pad 1 = 1, 2 = 2, 3 = 3, 4 = S, 5 = R, 6 = T, 7 = T_SW (single reading), 8 = G. Revision 2 wires both T pads to TRS_T, so which one the drawing calls T_SW no longer matters. Check the two T pads read as one node by continuity on the first sample.
3. Courtyards repaired with `--repair-courtyard` (vendor courtyard excluded the pads): NMJ6HCD2 26.61x18.29 -> 27.12x19.24 mm; OPA1678IDR 5.09x4.09 -> 5.60x7.88 mm; SS34 4.41x2.69 -> 6.90x3.20 mm; R0603 1.69x0.89 -> 2.82x1.40; C0603 1.69x0.89 -> 2.70x1.40; C1206 3.29x1.69 -> 5.18x2.24; HDR 1x3 repaired the same way.
4. SWD header C52016392 was refused (`no_courtyard`); C2691448 serves both OLED and SWD. No Basic 1x4 header exists on JLC. Spec item 14/16 note this.
5. AO4606 (C2944311) and SMBJ70CA (C224026) are dropped by the spec but still sit in cubevox.kicad_sym; remove with the schematic spec edit if wanted. cubevox.json still references PJ-611E and must be rewritten.
6. OPA2197IDR: pin types set by hand like OPA1678 (1,7 output; 2,3,5,6 input; 4,8 power_in); `assign_pin_types.py` leaves 0 unspecified. Pin order matches the OPA1678 symbol (1 OUTA, 2 -INA, 3 +INA, 4 V-, 5 +INB, 6 -INB, 7 OUTB, 8 V+). Datasheet not re-read in this pass.
7. Pin maps NOT VERIFIED: PJ-002A, NMJ6HCD2, OPA1678IDR symbol pin order. Axis candidates printed by the import are unverified.
8. Stocks: RK09D1130C1B 986 (15 per board), NCJ6FA-H 111. The local foreman catalogue shows C25804 at stock 0; live check shows 23,056,985 (catalogue stale).
9. Qty for BAT54S (2) and tact switches (2) are estimates until the schematic exists.
10. Pot and encoder shaft heights (spec item 17) are datasheet figures, not checked against the imported models. 22 uF is X5R (Basic); an X7R 22 uF would be Extended.
11. C1788487 ST-0-102-A01-T000-LF: import refused with model_courtyard_mismatch (left 1.30, top 2.85, right 2.60, bottom 2.85 mm). Cause: the .wrl holds the M5 hex nut, the dia 11 lock washer with its 12.3 mm tab, the lever (23.6 mm high) and the pins, so its plan extent is x -5.50..6.80, y +-5.49 mm against the 8.3 x 5.2 body courtyard. Body, pads (2.54 pitch) and model alignment match the datasheet; no offset or rotation was applied. Fix: imported with --accept-mismatch, then the four F.CrtYd lines were edited to x -5.75..7.05, y -5.75..5.75 (model plan + 0.25 mm). The silk outline (8.3 x 5.2) is unchanged. The footprint carries the washer tab on the +X side; the physical orientation of the tab is not set by the datasheet. Pin names COM/T1/T3 are mine (symbol was 1/2/3); pad 2 is the centre common. The symbol is stored as ST-0-102-A01-T000-LF+PJ.
12. Electrolytic polarity (revision 2). The three ROQANG footprints (EasyEDA C72484, C191859, C72502) put pad 1 on the -X side and mark that side with a plus-sign glyph, the chamfered body corners and a dot on F.SilkS, with a bare minus bar on the +X side. So pad 1 = +. The symbols draw the plus at the pin 1 end and pin 1 is the + pin, so the schematic and board agree. The 3D models were not checked for orientation.
13. Assembly (revision 2). J101 (combo in), J106 (barrel), J108 (1/4 in out) and J117 (OLED header and module) are owner-assembled and must be excluded from the JLC BOM and CPL. USB-C J1 is JLC-assembled. J117's LCSC field holds the text "owner-supplied module, hand-plugged", not an LCSC code, so a BOM export must filter it by reference.

## Dropped from fxbox

PJ-325M (C2884942), WS2812B-2020 (C52917434), B3F-4055 (C84931), RK09K pots (C470311, C209779), TPA6132A2 (C69901), AO4606 (C2944311), SMBJ70CA (C224026), NE5532DR (C7426), PJ-611E (C309282), unused probe headers C32713270 and C124378, TLV9062 (C398356), TPS63070 (C109322), LP5912 (C2761351).
