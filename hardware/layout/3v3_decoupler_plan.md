# 3V3 decoupler survey and trunk re-plan

Read-only survey against git HEAD b072bb2, ledger 0347. No copper or parts moved.

## Basis

1. Scope: every gate-63 row in ledger 0347 with a `/3V3 trace` leg.
2. Pin intent comes from `brief.confirmed.json` clusters (vdd_decap_west/south/east/north, adc_supply, dac_supply, buck33_out).
3. Clearance 0.2 mm (all netclasses), board minimum 0.15 mm, via 0.6/0.3.
4. U1 courtyard is a rectangle 87.41–104.59 × 75.41–92.59. Caps sit outside it.
5. Courtyard half-sizes: 0402 ±0.5 across × ±0.95 along; 0603 ±0.78 × ±1.53.
6. Pattern for a U1 pin pair: pin escape → 10 nF 0402 3V3 pad (1.55 mm) → 100 nF 0603 3V3 pad (2.85 mm), both pads on the escape line. The trunk enters the 0603's 3V3 pad from the far side. Gate-63 budget is 3.0 mm.
7. Unrouted signals whose escapes must stay reserved: TOGGLE2 (U1.98), TOGGLE3/TOGGLE4/MUX_S2 (U1.28–30), I2C_SDA/SCL, ENC_MENU_A/B (U1.44–47).
8. Every coordinate is first-cut and needs a DRC pass before copper.

## Cap table

| Cap | Pin | Today: pin→cap, what the trunk reaches first | Proposal: centre, rot → 3V3 pad / GND pad / GND via | Pin→cap after | Moves, clearances checked |
|---|---|---|---|---|---|
| C56 10n 0402 | U1.100 (88.45,90.0) | 1.97 straight; cap first, via (86.48,90.71) under it | (86.9,90.48) rot -90 → (86.9,90.0) / (86.9,90.96) / (86.9,91.75) | 1.55 | Courtyard x 86.4–87.4 clears U1's 87.41. The 3V3 via (86.48,90.71) is removed (trunk item 18). |
| C2 100n 0603 | U1.100 | 4.10 via the 3V3 via | (85.6,90.775) rot -90 → (85.6,90.0) / (85.6,91.55) / (85.6,92.35) | 2.85 | Courtyard 84.82–86.38 clears C56. Pad top 89.53 leaves 0.43 to the unrouted TOGGLE2 escape at y 89.0. GND via is 0.68 above R27's courtyard. |
| C58 10n 0402 | U1.27 (103.55,89.5) | 2.45; trunk reaches C3 first | (105.1,89.98) rot -90 → (105.1,89.5) / (105.1,90.46) / (105.1,91.25) | 1.55 | Its GND via moves off (105.2,90.46). |
| C3 100n 0603 | U1.27 | 5.03 | (106.4,90.275) rot -90 → (106.4,89.5) / (106.4,91.05) / (106.4,91.85) | 2.85 | Conflict: pad tops at 89.22 leave 0.12 to the unrouted TOGGLE3 escape at y 89.0. Pins 28–30 must fan NE at x ≤104.6. Today's C58 has the same problem. Old GND via (106.85,93.53) is removed. |
| C7 100n 0603 | U1.11 (95.0,91.55) | 8.7 away, 31.4 routed; a leaf of FB1's west branch | (95.0,94.5) rot -90 → (95.0,93.725) / (95.0,95.275) / (95.0,96.05) | 2.18 | Y1, C15 and C16 move south 2.0: Y1 (96.2,98.7), C15 (92.75,100.75), C16 (99.0,101.0). Via bottom 96.35 then sits 0.55 from Y1.1. HSE_IN/HSE_OUT (pins 12/13) chamfer 0.4 east at the pin tips, so HSE_IN runs at x 95.9 (0.33 clear of C7.1). C4.1 feeds C7.1 straight along y 93.725 (1.65). Brief says "plane-fed", but In1 and In2 are both GND, so no 3V3 plane exists. |
| C4 100n 0603 | U1.6 (92.5,91.55) | 2.89 chamfered; passes | stays (93.35,94.5) rot -90 | 2.89 | A straight slot at x 92.5 is blocked by the I2S_DATA via (92.25,93.6). |
| C61 10n 0402 | U1.50 (103.55,78.0) | 2.9 away, unrouted | (105.1,77.52) rot 90 → (105.1,78.0) / (105.1,77.04) / (105.1,76.3) | 1.55 | Needs decision 2. Courtyard 104.6–105.6 clears U1. |
| C5 100n 0603 | U1.50 | 25.6; on the bulk spine | (106.4,77.225) rot 90 → (106.4,78.0) / (106.4,76.45) / (106.4,75.65) | 2.85 | Courtyard 105.62–107.18 × 75.69–78.75 clears C61 and D3. C12 (VCAP1) moves to (108.71,79.0) rot 0: VCAP1 pad (107.93,79.0), GND (109.49,79.0). VCAP1 runs straight east 4.4 at y 79.0, 0.375 below C5/C61 pads. Pins 44–47 must fan SE before x 104.9. |
| C57 10n 0402 | U1.75 (90.0,76.45) | 2.60; passes | stays (88.25,74.0) | 2.60 | The protected 3V3 via (88.25,75.16) stays. |
| C1 100n 0603 | U1.75 | 4.36 | no F.Cu slot within 3 mm | — | Decision 1. |
| C47 100n 0603 | U8.4 (126.8,102.5) | 2.59; trunk reaches the pin first, from W under U8 | stays (130.0,101.5) rot 0 | 2.59 | Decision 3. |
| C46 10µ 0603 | U8.4 | 6.1, unrouted | (129.225,99.19) rot 90 → (129.225,99.965) / (129.225,98.415) / (129.225,97.6) | 4.1, still fails | Exactly 0.2 to the ADC_SCKI via (128.25,100.5). Moving C46 frees (133.5,104) for C33. |
| new Cx 100n 0402 | U6.20 (139.13,105.43) | no DVDD decoupler in the schematic | (137.01,105.43) rot 180 → (137.49,105.43) / (136.53,105.43) / (136.53,106.2) | 1.64 | Schematic add. C33 moves to (136.43,104.13) rot 180: LDOO pad (137.205,104.13), GND (135.655,104.13), GND via (134.85,104.13). Pin 18 runs straight 1.93. Courtyard gaps are tight: Cx–C33 0.02, C33–U6 0.00. |
| C6 4.7µ, C32 10µ, C31 100n, C59 10n | bulk at the 3V3 entry (brief vdd_decap_north) | 18.7–23.6 | stay on the north spine | — | Decision 4. |
| C21, C22 22µ | buck33 output caps (U3/L1) | 13.8 / 16.6 | stay | — | U3.1 (FB) is not a power_in pin, so gate 63 measures these to U1.75. Register a reason. |

## Decisions

1. **U1.75 second cap (C1).** VCAP2's C13 holds the x 90.0 slot north of pin 75. SWDIO (x 91.5) and USB_DP (x 92.0) box in pin 73, so C13 cannot move beside it. No F.Cu placement puts C1 within 3 mm. The options are:
   1. register C1 as bulk on the spine;
   2. delete C1 from the schematic;
   3. put C13 on B.Cu, which means double-sided assembly.
2. **U1.50 pair (C61, C5).** The in-line pair needs C12 (VCAP1) moved out to (108.71,79.0), so the VCAP1 trace grows to 4.4 mm. VCAP isn't gated by 63, but ST wants it close. Pins 44–47 must also fan SE before x 104.9. Accept this, or keep one cap at pin 50?
3. **U8.4 (C46, C47).** U8's pins sit on a 0.65 mm pitch, and its neighbours escape east (ADC_SCKI north, ADC_VCC south), so only one cap fits within 3 mm. The trunk can't reach C47 before the pin, because it is blocked by I2S_LRCLK at x 124.9 and the ADC_SCKI via at (128.25,100.5). Register C46 as bulk at (129.225,99.19) and keep the pin-first feed?
4. **Bulk caps.** Register C6/C32 as entry bulk and C21/C22 as buck output caps. C31 (100n) and C59 (10n) are listed as "bulk" in the brief. Are they wanted, and for which pin?
5. **U6.20 DVDD.** Add a 100 nF 0402 (Cx) to dac_supply in the schematic.

## Trunk re-plan

### North spine (F.Cu; C5 leaves the spine)

1. Remove the C5 chain: (104.2,74.275)-(104.7,73.775), (104.7,73.775)-(106,73.775), (106,73.775)-(106.35,73.775), (106.35,73.775)-(108.15,71.975), (108.15,71.975)-(108.3,71.975).
2. F.Cu C32.1 (103.86,74.275) → (104.2,74.275), a stub inside the pad (kept).
3. F.Cu (104.2,74.275) → (106.5,71.975), a diagonal that clears C32.2's corner by 0.51.
4. F.Cu (106.5,71.975) → R10.1 (108.3,71.975).

### U1.50 feed (F.Cu, from C32.1's free south side)

5. F.Cu C32.1 (103.86,74.275) → (103.86,74.65), a stub inside the pad.
6. F.Cu (103.86,74.65) → (104.21,75.0).
7. F.Cu (104.21,75.0) → (106.9,75.0). This leaves 0.2 to C5's GND via and 0.5 to the top-row pin tips.
8. F.Cu (106.9,75.0) → (107.275,75.375).
9. F.Cu (107.275,75.375) → (107.275,77.55), 0.475 clear of D3.1 and 0.575 clear of the GND via (108.3,77.72).
10. F.Cu (107.275,77.55) → (106.825,78.0), ending inside C5.1's copper.
11. F.Cu (106.825,78.0) → C5.1 (106.4,78.0).
12. F.Cu U1.50 (103.55,78.0) → C61.1 (105.1,78.0) → C5.1 (106.4,78.0).

Items 6–10 hold two short jogs. Gate 136 passes them only through its fan-out exception, because the outer diagonals end in pad copper.

### FB1 island tie (FB1, C3, C58, U1.27, U8.4, C47)

13. Split the F.Cu y 71.975 trunk at a new via **V1 (109.5,71.975)**. F.Cu runs straight through it (180°), and it sits 0.425 from R10.1.
14. **B.Cu** V1 (109.5,71.975) → **V2 (109.5,91.0)**. The B.Cu column x 108.6–109.8, y 71–92 is clear except GND vias at (108.3,77.72) and (108.3,78.6), which are 0.45 away.
15. F.Cu (102.6,100.0) → (109.1,100.0), then (109.1,100.0) → (109.5,99.6). This replaces the run to (108.8,100.0) and the diagonal to (109.2,99.6).
16. F.Cu (109.5,99.6) → V2 (109.5,91.0) → (109.5,89.95). F.Cu runs straight through V2, and the riser sits 0.55 from C10's GND via. This replaces the x 109.2 riser, (109.2,92.4)-(108.785,91.985) and (108.785,91.985)-C3.1.
17. F.Cu (109.5,89.95) → (109.05,89.5), then (109.05,89.5) → C3.1 (106.4,89.5).
18. F.Cu C3.1 → C58.1 (105.1,89.5) → U1.27 (103.55,89.5).
19. Remove C3.1's old north stub chain to C58.1: (106,91.985)-(106,91.6)-(106.7,90.9)-(106.7,89.95)-(106.25,89.5)-(106,89.5).
20. Remove FB1's west branch to C7: (101,98.8)-(100.6,98.8)-(100,99.4)-(100,100.2)-(99.5,100.7)-(96.2,100.7)-(95.725,100.225)-(94.75,100.225). FB1.1 keeps its south and east branches.

### U1.100

21. Remove the 3V3 via (86.48,90.71) and the B.Cu run (86.48,90.71)–(83.11,90.71), plus its tail to (82.75,90.35).
22. B.Cu (82.75,79.95) → **V3 (82.75,90.0)**, a straight entry of 10.05.
23. F.Cu V3 (82.75,90.0) → C2.1 (85.6,90.0) → C56.1 (86.9,90.0) → U1.100 (88.45,90.0).

### U1.6 / U1.11

24. F.Cu U1.11 (95.0,91.55) → C7.1 (95.0,93.725), straight south.
25. F.Cu C4.1 (93.35,93.725) → C7.1 (95.0,93.725), straight east.
26. Remove U1.11's old link (95,91.55)-(95,92.65)-(94.75,92.9)-(94.3,92.9) and its branch to C4.1, then re-lay U1.6 → C4.1 alone with today's chamfered path.

### U6.20

27. F.Cu (139.13,123.47) → (139.13,108.0). This shortens today's segment, which ends at U6.20.
28. F.Cu (139.13,108.0) → (137.49,106.36).
29. F.Cu (137.49,106.36) → Cx.1 (137.49,105.43). Cx's GND via is 0.51 clear.
30. F.Cu Cx.1 (137.49,105.43) → U6.20 (139.13,105.43).
31. F.Cu U6.18 (139.13,104.13) → C33.1 (137.205,104.13), replacing today's LDOO chamfer chain.

SW104's pads near item 28 are not yet checked.

### U8.4

32. Unchanged pending decision 3. The trunk arrives at the pin from W under U8: (123.1,107.6) → (124.6,106.1) → (124.6,102.9) → (125.0,102.5) → U8.4.

## Open ties

1. The U1.6/U1.11 cluster is fed only by the run from U1.100 under the package: (88.45,90.0) → (92.0,90.0) → pin 6. So the trunk reaches pin 6 before C4. Every alternative entry into C4.1 is blocked by the I2S_DATA via (92.25,93.6) or by the pin row.
2. The U6.20 island (U6.20, R137.1) has no tie to the buck. Its nearest main-trunk point is the y 71.975 / x 134.51 run, more than 30 mm away.
3. After items 13–16, the FB1 island ties to main through V1/V2. The ledger's 3V3 unconnected count should then drop. Verify that with DRC at execution.
