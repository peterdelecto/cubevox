# Codex round 3a: placement questions (pre-routing)

Reply in at most 120 tokens total. One line per item: PASS / FAIL / UNSURE plus at most eight words. No preamble, no restating.

Board: STM32H743 LQFP-100, 4-layer JLC, single-side SMD. Rear jacks and panel controls fixed by the owner.

1. AP63203 SW net: pin 5 to C23 (BST) to L1 pad, pad-centre MST 6.03 mm, copper will be about 4 mm. Budget was 6 mm. Accept 8 mm?
2. USB FS pair J1 to U1: airline 48 mm, route will land 50 to 55 mm, 2 vias, USBLC6 at the connector. Budget was 50 mm. Accept 60 mm?
3. VDD pin 11 sits over the crystal courtyard. It gets a 100 nF 7 mm away fed from the 3V3 inner plane by via, plus a 10 nF moved to the north row. Five other VDD pins have 10 nF plus 100 nF within 1.2 to 4.5 mm. Acceptable?
4. NRST and the pot-mux ADC input (pin 15) leave by immediate vias to B.Cu between the crystal and the 0 R series part, anti-alias RC 6 mm from the pin with 2 vias. Acceptable?
5. HSE: crystal 2.2 mm from pin 12, load caps 2.3 mm (pin side) and 1.9 mm (R1 side) stubs, geometrically asymmetric by 3 mm. Acceptable?
6. 3V3A from the LDO north of the MCU to the VDDA ferrite south of it, about 35 mm on an inner layer. Acceptable?
