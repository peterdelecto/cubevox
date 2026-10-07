# cubevox PARTS

Board: cubevox main board (one JLC-assembled board). Spec: docs/specs/2026-10-02-hardware-design.md.
Snapshot 2026-10-02. Prices are the 10+ tier. Stock is from `jlc_get_part` unless marked LIVE
(`jlc_stock_check`, 2026-10-02). Re-check everything at order time.
Build quantity: 1 board (qty column is per board; required = per-board x N + allowance, 17 pots x N).

## Parts

| # | LCSC | MPN | Role | Tier | Stock | $ @10+ | Footprint (lib) | Qty/bd | Import verdict |
|---|------|-----|------|------|-------|--------|-----------------|--------|----------------|
| 1 | C114409 | STM32H743VIT6 | MCU | Extended | 3681 | 10.69 | LQFP-100 (h7core) | 1 | inherited, in lib |
| 2 | C9006 | X322525MOB4SI | 25 MHz crystal | Basic | 199401 | 0.076 | CRYSTAL-SMD_4P 3.2x2.5 (h7core) | 1 | inherited |
| 3 | C393939 | TYPE-C16PIN | USB-C | Extended | 397977 | 0.055 | USB-C-SMD_TYPE-C16PIN (h7core) | 1 | inherited |
| 4 | C2687116 | USBLC6-2SC6 | USB ESD | Extended | 82399 | 0.038 | SOT-23-6 (h7core) | 1 | inherited |
| 6 | C2071056 | AP63205WU-7 | 5 V buck | Extended | 36399 | 0.319 | TSOT-23-6 (cubevox) | 1 | inherited |
| 7 | C7427135 | ANR5040T4R7M | AP63205 inductor 4.7 uH | Extended | 17676 | 0.043 | IND-SMD 5x5 (cubevox) | 1 | inherited |
| 8 | C780769 | AP63203WU-7 | 3V3 buck | Extended | 17725 | 1.054 | TSOT-26 (h7core) | 1 | inherited |
| 9 | C7427132 | ANR5040T3R9M | AP63203 inductor 3.9 uH | Extended | 1235 | 0.050 | IND-SMD 5x5 (h7core) | 1 | inherited |
| 10 | C404027 | TLV75533PDBVR | 3V3A LDO | Extended | 136560 | 0.113 | SOT-23-5 (h7core) | 1 | inherited |
| 11 | C55513 | PCM1808PWR | ADC | Extended | 27700 | 0.563 | TSSOP-14 (h7core) | 1 | inherited |
| 12 | C107671 | PCM5102APWR | DAC | Extended | 6661 | 1.276 | TSSOP-20 (h7core) | 1 | inherited |
| 13 | C47190 | G6K-2F-Y-TR DC5 | output mute relay K102 (pole B grounds the tip while muted), DPDT 5 V | Extended | 68587 | 0.915 | RELAY-SMD_G6K-2F (cubevox) | 1 | inherited; K101 bypass relay removed 2026-10-07 (bypass became MUTE) |
| 14 | C3096082 | PJ-002A | 9 V barrel jack | Extended | 1893 | 0.753 | DC-IN-TH_PJ-002A (cubevox) | 1 | inherited |
| 16 | C19077570 | SMBJ15CA | 9 V TVS | Preferred | 150573 | 0.044 | SMB (cubevox) | 1 | inherited |
| 17 | C98457 | CD74HC4067SM96 | 16:1 pot mux | Extended | 9499 | 0.460 | SSOP-24 (cubevox) | 1 | inherited |
| 18 | C470754 | EC11E15244B2 | MENU encoder, with switch (1.5 mm travel) | Extended | 5660 (2026-10-03) | 2.04 (10+) | SW-TH_EC11E (cubevox, Alps layout) | 1 | owner 2026-10-03, matches C2P pot; model EC11E15244B2_from_alps_ec11n.step = Alps EC11N1525404 STEP with shaft +5 mm, measured tip 24.5 |
| 19 | C470710 | EC11E15204A3 | KEY, SEMITONES encoders (removed 2026-10-07, do not order: both are pots now) | Extended | 847 (2026-10-03) | 2.24 (10+) | SW-TH_EC11E (cubevox, Alps layout) | 0 | owner 2026-10-03; model EC11E15204A3_from_alps_ec11n.step = Alps EC11N1520401 STEP with shaft +5 mm, measured tip 24.5 |
| 20 | C361173 | RK09D1130C2P | pots, 16 slot knobs on the mux + Input Gain | Extended | 174 (2026-10-03) | 0.629 (10+) | RES-ADJ-TH_RK09D1130C2P (cubevox) | 17 | owner 2026-10-03; Adam 2026-10-07 8 slots × 2; Alps drawing No. 4 shared with C1B, LM1 25. Model RK09D1130C2P_from_alps_c1b.step = Alps C1B STEP with shaft +5 mm in the D-flat, measured tip 25.0, collar 11.8. EasyEDA's C2P/C3C models are swapped |
| 20b | C470304 | RK09D1130C1B | pots (superseded 2026-10-03, do not order) | Extended | 986 | 0.84 (10+) | RES-ADJ-TH_RK09D1130C3C (cubevox) | 0 | replaced by C361173 (owner wants the 25 mm shaft); its Alps STEP (tip 20.0) is the base of the C2P model |
| 21 | C368458 | NCJ6FA-H | XLR + 1/4" combo input | Extended | 111 (LIVE) | 2.600 | CONN-TH_NCJ6FA-H (cubevox) | 1 | IMPORTED with --model-offset -1.24 -11.46 0 and --accept-mismatch (catalogue 3P is the XLR pin count), see WARNINGS 1 |
| 22 | C309282 | PJ-611E | 1/4" output jack (HOOYA) | Extended | 1562 (LIVE 2026-10-02) | 0.541 | AUDIO-TH_PJ-611E_1 (cubevox, copied from fxbox with its model offset 2.84/-2.85) | 1 | owner 2026-10-02 replaces NMJ6HCD2 (C368502); pin map 2 sleeve, 4 ring, 6 tip, 3/5/7 switch contacts, verified against the HOOYA drawing in the fxbox record |
| 24 | C19726 | BAT54SLT1G | preamp input clamps | Extended | 256249 | 0.019 | SOT-23 (h7core) | 2 (est.) | inherited |
| 25 | C318884 | TS-1187A-B-A-B | BOOT0, NRST tact | Basic | 473784 | 0.016 | SW-SMD_4P 5.1x5.1 (h7core) | 2 (est.) | inherited |
| 28b | C42379197 (socket); module owner-supplied | hanxia HX PM2.54-1x4P TP-YQ socket + SH1106 1.3in I2C 128x64 module | J117: 5 mm 1x4 SMD female socket, JLC-assembled (SMD so it sits square; THT headers came out crooked, owner 2026-10-06); the OLED module hand-plugs into it | extended | 0.22 | 17414 | OLED-1.3-SH1106-I2C-4P (cubevox, hand-written; socket pads staggered ±1.80 from the row, 1.27x2.20) | 1 | Module PCB 35.4x33.5x1.2, pins GND/VCC/SCL/SDA, 4x NPTH 3.2 on a 30.4x28.5 grid. Buy 4x M3 x 9 mm standoffs (plus 4 screws). Model assumes 9.0 mm standoffs: module PCB bottom at z 9.0, glass top at z 11.7. With 9 mm standoffs the socket takes 6.5 mm of pin (spec item 22). The 11.8 mm panel plane needs about 9.1 mm standoffs. |
| 29 | C192421 | OPA1678IDR | SUPERSEDED by row 50 (revision 2, audit A02: input range stops 2 V below V+). Do not order | Extended | 3309 | 0.774 | SOIC-8_L5.0-W4.0 (cubevox) | 0 | footprint kept in cubevox.pretty, unused |
| 30 | C8678 | SS34 | 9V_JACK to VIN_9V Schottky | Basic | 3557042 | 0.030 | SMA_L4.3-W2.6 (cubevox) | 1 | IMPORTED with --repair-courtyard |
| 32 | C81598 | 1N4148W | relay flyback D109 (mute) | Basic | 5171770 | 0.012 | SOD-123F (cubevox) | 1 | inherited; D104 removed with K101, 2026-10-07 |
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
| 50 | C139363 | OPA2197IDR | A1-A4 (U104, U105), BIAS_W follower + output buffer (U106, supplied from 5VA_W through FB102): 3 duals | Extended | 9501 (import 2026-10-02; spec quotes 19.8 k live) | 0.8265 (import; spec quotes 1.26) | SOIC-8_L4.9-W3.9-P1.27-LS6.0-BL (cubevox) | 3 | IMPORTED with --repair-courtyard (4.99x3.99 -> 5.50x7.50 mm); model offset clean, no --accept-mismatch. Pin types by hand like OPA1678 (1,7 output; 2,3,5,6 input; 4,8 power_in); 0 unspecified |
| 51 | C309083 | ARG03BTC1002 | R111-R114, difference amp, 10 k 0.1 % 25 ppm | Extended | 59843 | 0.0214 | R0603 (cubevox) | 4 | IMPORTED with --repair-courtyard; same R0603 footprint as the other 0603 resistors |
| 52 | C72484 | RVT1E100M0405 | 10 uF 25 V electrolytic 4x5.4 (ROQANG, 105 C): C121, C125 coupling, C139 DAC, C141 output, C160 VREF node | Extended | 32285 | 0.0217 | CAP-SMD_BD4.0-L4.3-W4.3-FD (cubevox) | 5 | IMPORTED with --repair-courtyard (4.39x4.39 -> 6.62x4.90 mm). Symbol polarised, pin 1 = +. Footprint pad 1 = + (see WARNINGS 12) |
| 53 | C191859 | VT1A101M0505 | 100 uF 10 V electrolytic 5x5.4: C159 (stage 2 R116 leg) | Extended | 26521 | 0.0256 | CAP-SMD_BD5.0-L5.3-W5.3-LS6.3-FD (cubevox) | 1 | IMPORTED with --repair-courtyard (5.39x5.39 -> 8.00x5.90 mm). Pin 1 = + |
| 55 | C23179 | 0603WAF4700T5E | R 470 ohm: R147, DAC reconstruction filter | Basic | 1899614 | 0.0016 | R0603 | 1 | IMPORTED, courtyard repaired (same footprint) |
| 57 | C23345 | 0603WAF220JT5E | R 22 ohm: R146, VREF follower isolation | Basic | 2657294 | 0.0017 | R0603 | 1 | IMPORTED, courtyard repaired |
| 60 | C90540 | 0603B105K500NT | C 1 uF X7R 50 V: C18, USB-C VBUS bypass beside J1/D2 | Extended | 918076 | 0.0368 | Capacitor_SMD:C_0603_1608Metric | 1 | stock from search 2026-10-02; replaces the inherited X5R C15849 |
| 61 | C72521 | RVT1E470M0605 | 47 uF 25 V electrolytic 6.3x5.4 (ROQANG, 105 C): C120, C124 mic coupling | Extended | 139544 (import 2026-10-02; search DB 67202) | 0.0319 | CAP-SMD_BD6.3-L6.6-W6.6-LS7.6-FD (cubevox) | 2 | IMPORTED with --repair-courtyard (6.69x6.69 -> 9.34x7.20 mm). Pin 1 = + by the family convention; pad 1 sits on -X like the 4x5.4 parts. Silk polarity mark and 3D orientation not checked |
| 62 | C413679 | EEEHP1E220P | 22 uF 25 V bipolar (non-polarised) electrolytic 6.3x5.8 (Panasonic): C158 stage 1 RG leg | Extended | 835 (import 2026-10-02; search DB 660) | 0.3059 | CAP-SMD_BD6.3-L6.6-W6.6-FD (cubevox) | 1 | IMPORTED with --repair-courtyard (6.69x6.69 -> 9.34x7.20 mm). Schematic uses Device:C; the imported symbol draws a polarity mark and is unused |
| 63 | C20526 | MMBT3904 | Q101 NPN, K102 coil driver (R150 1 k base, R153 100 k base pull-down) | Basic | 2033567 (import) | 0.0108 | SOT-23-3_L2.9-W1.3-P1.90-LS2.4-BR (cubevox) | 1 | IMPORTED with --repair-courtyard (1.39x2.99 -> 3.50x3.50 mm). Shares the footprint file of the unused 2N7002 (C8545). Pins B 1, E 2, C 3 |
| 64 | C25971 | 0603WAF4531T5E | R 4.53 k: R108, R110, stage 1 feedback (replaces 9.53 k C23127) | Extended | 15479 | 0.0027 | R0603 | 2 | Same footprint as the other 0603 resistors. No Basic 4.53 k exists |
| 65 | C31850 | 0603WAF2202T5E | R 22 k: R102, R105, TRS pad series | Basic | 3702668 | 0.0041 | R0603 | 2 | Same footprint |
| 66 | C4216 | 0603WAF3302T5E | R 33 k: R6, VBUS sense divider top | Basic | 3237413 | 0.0022 | R0603 | 1 | Same footprint |
| 67 | C23254 | 0603WAF8202T5E | R 82 k: R7, VBUS sense divider bottom | Basic | 1318029 | 0.0027 | R0603 | 1 | Same footprint |
| 68 | C23138 | 0603WAF3300T5E | R 330 ohm: R116, stage 2 gain leg (replaces 200 ohm C8218) | Basic | 3273227 | 0.0026 | R0603 | 1 | Same footprint |
| 69 | C412495 | SP809EK-L-2-9/TR | U107, LDO enable supervisor: 2.9 V threshold on 3V3, 230 ms release, drives U4 EN via LDO_EN | Extended | 16416 | 0.324 | Package_TO_SOT_SMD:SOT-23 | 1 | Open-drain, active low; holds 3V3A off until 3V3 is good (PINMAP Firmware item 6) |
| 70 | C221351 | OPA197IDBVR | U108, VREF buffer: drives VREF_BUF, then R146 22 ohm into the VREF node | Extended | 84235 | 0.8373 (10+) | Package_TO_SOT_SMD:SOT-23-5 | 1 | Supplied from 5VA; 5VA_W (FB102) supplies U106 only |

## Counts

1. Unique part numbers: 58 (26 Basic, 1 Preferred, 31 Extended). Row 29 (OPA1678) is superseded and not counted. Row numbers are not contiguous (5, 15, 23, 26, 27, 28, 31, 54, 56, 58, 59 removed).
2. Fee estimate: 31 Extended x ~$3 = ~$93 per order. NCJ6FA-H is counted although it is not in the library yet.
3. Per-board quantities fixed: 3x OPA2197, 1x OPA197, 17 pots (16 slot pots RK09D1130C2P + RVGAIN1), 1 encoder (MENU, EC11E15244B2), 9 bat toggles, 1 relay (K102), 1 SS34. Passive quantities come from the schematic.
4. Not yet coded (spec items): 10 ohm PCM1808 VCC resistor, 2.2 uF VCAP caps, CC 5.1 k C23186, BLM18PG121SN1D ferrite, the toggle switch itself (header placeholders only), clamp C27675.

## Assembly

| Ref | Part | Assembled by |
|-----|------|--------------|
| J101 | NCJ6FA-H combo input (B.Cu) | owner, excluded from JLC BOM/CPL |
| J106 | PJ-002A barrel (B.Cu) | owner, excluded |
| J108 | PJ-611E 1/4 in output (B.Cu) | owner, excluded |
| J117 | hanxia socket C42379197 JLC-assembled; SH1106 OLED module | socket on BOM/CPL; module owner-plugged |
| all other refs | | JLC (top side, Economic) |

## WARNINGS

1. C368458 NCJ6FA-H is imported. ClaudeRouter `pad_roles` now takes the card branch when `mechanical_pads` is present, even empty (uncommitted R change, awaiting owner review), so 8 numbered pads resolve as contacts. `pin_count_mismatch` (catalogue 3 vs 8) is accepted with the reason "catalogue 3P is the XLR pin count; footprint has 8 contacts per Neutrik ST-NCJ6FAH". `--model-offset -1.24 -11.46 0` clears the model overhang. Symbol pins are named 1, 2, 3, S, R, T, T_SW, G (pads 1-8), all passive. Revision 2: pads 6 and 7 are both the tip (Neutrik electrical diagram, audit A01); both go to TRS_T and there is no plug detection. Stock 385 live 2026-10-02 pm. NCJ6FA-V C368485 is NOT a fallback (vertical mount, wrong angle; owner 2026-10-02). ASSEMBLY: J101 (combo), J108 (1/4" out) and J106 (barrel) sit on B.Cu and are hand-soldered by the owner; exclude them from JLC's BOM/CPL. USB-C J1 stays top, JLC-assembled.
2. Pad map from the Neutrik drawing ST-NCJ6FAH, matched by position (footprint x is mirrored against the component-side drawing): pad 1 = 1, 2 = 2, 3 = 3, 4 = S, 5 = R, 6 = T, 7 = T_SW (single reading), 8 = G. Revision 2 wires both T pads to TRS_T, so which one the drawing calls T_SW no longer matters. Check the two T pads read as one node by continuity on the first sample.
3. Courtyards repaired with `--repair-courtyard` (vendor courtyard excluded the pads): NMJ6HCD2 26.61x18.29 -> 27.12x19.24 mm; OPA1678IDR 5.09x4.09 -> 5.60x7.88 mm; SS34 4.41x2.69 -> 6.90x3.20 mm; R0603 1.69x0.89 -> 2.82x1.40; C0603 1.69x0.89 -> 2.70x1.40; C1206 3.29x1.69 -> 5.18x2.24; HDR 1x3 repaired the same way.
4. SWD is on the Tag-Connect TC2030-NL pads (J3); no header part. The OLED module plugs into J117, a JLC-assembled hanxia 5 mm SMD socket (C42379197).
5. AO4606 (C2944311) and SMBJ70CA (C224026) are dropped by the spec but still sit in cubevox.kicad_sym; remove with the schematic spec edit if wanted. cubevox.json still references PJ-611E and must be rewritten.
6. OPA2197IDR: pin types set by hand like OPA1678 (1,7 output; 2,3,5,6 input; 4,8 power_in); `assign_pin_types.py` leaves 0 unspecified. Pin order matches the OPA1678 symbol (1 OUTA, 2 -INA, 3 +INA, 4 V-, 5 +INB, 6 -INB, 7 OUTB, 8 V+). Datasheet not re-read in this pass.
7. Pin maps NOT VERIFIED: PJ-002A, NMJ6HCD2, OPA1678IDR symbol pin order. Axis candidates printed by the import are unverified.
8. Stocks: RK09D1130C2P 174 (17 per board), NCJ6FA-H 111. The local foreman catalogue shows C25804 at stock 0; live check shows 23,056,985 (catalogue stale).
9. Qty for BAT54S (2) and tact switches (2) are estimates until the schematic exists.
10. Pot and encoder shaft heights (spec item 17) are datasheet figures, not checked against the imported models. 22 uF is X5R (Basic); an X7R 22 uF would be Extended.
11. C1788487 ST-0-102-A01-T000-LF: import refused with model_courtyard_mismatch (left 1.30, top 2.85, right 2.60, bottom 2.85 mm). Cause: the .wrl holds the M5 hex nut, the dia 11 lock washer with its 12.3 mm tab, the lever (23.6 mm high) and the pins, so its plan extent is x -5.50..6.80, y +-5.49 mm against the 8.3 x 5.2 body courtyard. Body, pads (2.54 pitch) and model alignment match the datasheet; no offset or rotation was applied. Fix: imported with --accept-mismatch, then the four F.CrtYd lines were edited to x -5.75..7.05, y -5.75..5.75 (model plan + 0.25 mm). The silk outline (8.3 x 5.2) is unchanged. The footprint carries the washer tab on the +X side; the physical orientation of the tab is not set by the datasheet. Pin names COM/T1/T3 are mine (symbol was 1/2/3); pad 2 is the centre common. The symbol is stored as ST-0-102-A01-T000-LF+PJ.
12. Electrolytic polarity (revision 2). The three ROQANG footprints (EasyEDA C72484, C191859, C72502) put pad 1 on the -X side and mark that side with a plus-sign glyph, the chamfered body corners and a dot on F.SilkS, with a bare minus bar on the +X side. So pad 1 = +. The symbols draw the plus at the pin 1 end and pin 1 is the + pin, so the schematic and board agree. The 3D models were not checked for orientation.
13. Assembly (revision 2). J101 (combo in), J106 (barrel), J108 (1/4 in out) are owner-assembled and must be excluded from the JLC BOM and CPL. USB-C J1 and the OLED socket J117 (C42379197) are JLC-assembled; the OLED module itself is owner-plugged.

## Dropped from fxbox

PJ-325M (C2884942), WS2812B-2020 (C52917434), B3F-4055 (C84931), RK09K pots (C470311, C209779), TPA6132A2 (C69901), AO4606 (C2944311), SMBJ70CA (C224026), NE5532DR (C7426), unused probe headers C32713270 and C124378, TLV9062 (C398356), TPS63070 (C109322), LP5912 (C2761351), IS25LP064A QSPI flash (C2841348, owner 2026-10-02: settings in internal flash bank 2), TPS2116DRLR power mux (C3235557) and its 36 k PR1 resistor (C23147, owner 2026-10-02: 9 V only, USB is data only).
