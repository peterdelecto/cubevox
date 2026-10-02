# Codex round 3a: placement questions (pre-routing)

Reply in at most 120 tokens total. One line per item: PASS / FAIL / UNSURE plus at most eight words. No preamble, no restating.

Board: STM32H743 LQFP-100, 4-layer JLC, single-side SMD. Rear jacks and panel controls fixed by the owner.

1. AP63203 SW net: pin 5 to C23 (BST) to L1 pad, pad-centre MST 6.03 mm, copper will be about 4 mm. Budget was 6 mm. Accept 8 mm?
2. USB FS pair J1 to U1: airline 48 mm, route will land 50 to 55 mm, 2 vias, USBLC6 at the connector. Budget was 50 mm. Accept 60 mm?
3. VDD pin 11 sits over the crystal courtyard. It gets a 100 nF 7 mm away fed from the 3V3 inner plane by via, plus a 10 nF moved to the north row. Five other VDD pins have 10 nF plus 100 nF within 1.2 to 4.5 mm. Acceptable?
4. NRST and the pot-mux ADC input (pin 15) leave by immediate vias to B.Cu between the crystal and the 0 R series part, anti-alias RC 6 mm from the pin with 2 vias. Acceptable?
5. HSE: crystal 2.2 mm from pin 12, load caps 2.3 mm (pin side) and 1.9 mm (R1 side) stubs, geometrically asymmetric by 3 mm. Acceptable?
6. 3V3A from the LDO north of the MCU to the VDDA ferrite south of it, about 35 mm on an inner layer. Acceptable?

## Codex reply (owner relayed, 2026-10-02)

1. PASS. Compact 4 mm copper; keep bootstrap loop tight. -> brief BUCK_SW budget 6 -> 8 mm (owner to confirm at the placement stop).
2. PASS. 60 mm acceptable; maintain pair geometry and ground reference. -> brief USB_DP/DM budget 50 -> 60 mm (owner to confirm).
3. UNSURE. AN4938 section 9.3: decouplers close to power/ground pins, short wide connections to planes, no numeric limit. 7 mm alone neither passes nor fails; needs pin-to-cap path and ground return geometry. -> pin 11 and pin 10 drop vias straight to the In2 3V3 and In1 GND planes within 1 mm of the pads; C7 sits 7 mm away on the same two planes with its own two vias. Recorded as the pin 11 loop; re-ask with that geometry only if the owner wants.
4. UNSURE. Needs crystal clearance, ground continuity, ADC settling. -> NRST and pin 15 vias sit 0.1 mm outside the crystal courtyard, In1 GND is solid under the whole block, the pot-mux ADC input carries 100 R + 1 nF at 6 mm (tau 100 ns, sample time set in firmware).
5. PASS. Asymmetry acceptable; minimise loops, verify crystal loading on the bench.
6. PASS. With adequate width and local VDDA decoupling (FB1, 2 x 1 uF, 2 x 100 nF at pins 20/21).
