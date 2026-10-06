# Codex round 4: routing audit (groups 1 to 4, VBUS loop)

Reply in at most 120 tokens total. One line per item: PASS / FAIL / UNSURE plus at most eight words. No preamble, no restating.

Board: STM32H743 LQFP-100, 4-layer JLC (F.Cu, In1 GND, In2 GND, B.Cu), single-side SMD, 3V3 routed as traces not a plane.

1. 3V3 trunk cut west of U1. VDD pin 100 group and the DAC digital 3V3 now fed by a 0.3 mm, 19 mm F.Cu link under the U1 body from pin 75 to pin 11. Pin 100 keeps 10 nF at 1.55 mm and 100 nF at 2.85 mm. Pin 11 keeps 100 nF at 2.2 mm. Acceptable?
2. NRST: exits pin 14 by a via under the body, runs B.Cu, hops over a 3V3 B.Cu trace with a via pair, then F.Cu to the Tag-Connect and B.Cu to the 100 nF. Three vias on a pull-up only reset line. Acceptable?
3. Pot-mux ADC input (pin 15): via under the body, RC filter (R30 100 R, C55 1 nF) moved to 2 to 4 mm from the pin, line stays 1.65 mm from MUX_S2 and 1.65 mm from I2S LRCLK on the same layer, elsewhere on the opposite outer layer. Acceptable?
4. USB VBUS is sense only (C18, ESD array, R6/R7 divider to an MCU pin). Routed as a 0.2 mm F.Cu loop around the USB-C shell pads at 3.3 mm from the board edge, nested outside the CC2 loop, no vias. Acceptable?
5. Mux select lines S0 to S3 as one B.Cu bundle at 0.65 mm pitch, 50 mm to the CD74HC4067, each signal via with a GND return within 2 mm. Acceptable?
6. XSMT pull resistor R16 moved 35 mm to sit on the XSMT run beside U1 instead of at the DAC. Acceptable?

## Codex reply (gpt-6-astra, codex exec read-only, 2026-10-06, 24,585 tokens)

1. PASS. Retain local decoupling; check voltage drop. -> link is 0.3 mm x 19 mm, about 30 mOhm; a few mV at this group's current. Pin 100's 10 nF and 100 nF stay within 2.85 mm.
2. PASS. Three vias acceptable; keep capacitor near NRST. -> C14 (100 nF) sits on the B.Cu leg from the hub via at (81.12, 86.25).
3. PASS. Spacing acceptable; allow ADC settling. -> tau 100 ns with R30 100 R and C55 1 nF; sample time set in firmware.
4. PASS. Provided ESD protection remains connector-local. -> D2 stays at J1, unchanged.
5. PASS. Maintain continuous ground references; avoid fastest GPIO slew. -> In1 and In2 GND solid under the bundle; set the four select pins to low GPIO speed in firmware.
6. PASS. XSMT pull placement acceptable with continuous connection. -> R16 now on the run at (112.825, 92).
