# Codex round 3a: pre-routing placement findings

Date: 2026-10-02

Board: STM32H743 LQFP-100, four-layer JLC stackup, single-side SMD. Rear jacks and panel controls are fixed by the owner.

These are placement judgments based on the supplied geometry, not verification of completed routing or hardware. The jack and detailed pin-11 follow-up were researched with Sol and validated by Astra.

## Initial placement review

See [placement questions and owner-relayed notes](2026-10-02-round3-placement-questions.md).

| Item | Verdict | Finding |
| --- | --- | --- |
| AP63203 SW: 6.03 mm pad-centre MST, approximately 4 mm copper | Conditional PASS | The described compact placement is acceptable with a tight bootstrap loop and small SW copper area. An 8 mm bookkeeping budget is not a manufacturer-specified allowable trace length or blanket approval of an 8 mm routed SW node. |
| USB FS: expected 50–55 mm route, two vias, connector-side USBLC6 | Conditional PASS | A 60 mm placement budget is reasonable. Preserve pair geometry and a continuous ground reference through routing and layer transitions. |
| VDD pin 11: 100 nF at 7 mm, remote 10 nF | FAIL following clarification | Initially UNSURE without return-path geometry. The detailed plane geometry below warrants moving the 100 nF close to pins 10/11. |
| NRST and pot-mux ADC escape: immediate bottom-layer vias, RC 6 mm away | UNSURE | Requires review of crystal clearance, ground continuity, and ADC settling. Owner-relayed notes specify 0.1 mm outside the crystal courtyard, solid In1 ground, and 100 ohm / 1 nF filtering. These notes do not establish electrical clearance from the oscillator or verify acquisition and mux-settling timing. |
| HSE: crystal 2.2 mm from pin 12, capacitor stubs 2.3 / 1.9 mm, 3 mm asymmetry | Conditional PASS | Geometric symmetry is not itself a requirement. Keep oscillator loops compact and isolated, and verify crystal loading and startup margin. |
| LDO-to-VDDA ferrite supply: approximately 35 mm on an inner layer | Conditional PASS | Acceptable with adequate conductor width, a sound return path, and local VDDA decoupling after the ferrite. |

## Neutrik NCJ6FA-H mechanical follow-up

The manufacturer's ST-NCJ6FAH drawing gives:

- **6.35 mm** from the housing's panel-seating face to the nearest PCB pin/hole row.
- **5.7 mm** total forward projection to the barrel front from that face. This is the combined front projection, not a separate dimension for each flange and barrel feature.
- If the model's front datum is the physical barrel front, a PCB edge **6.9 mm** behind it lies **1.2 mm behind the panel-seating face**: 6.9 − 5.7 = 1.2 mm. It is therefore not coincident with that face.

The last conclusion is conditional on the model datum. A CAD origin or bounding-box front, especially one including the latch, need not equal the barrel front. The actual model datum was not inspected.

Source: [Neutrik NCJ6FA-H drawing, ST-NCJ6FAH](https://www.neutrik.com/media/8443/download/Drawing%20NCJ6FA-H.pdf?v=4).

## STM32H743 pin-11 decoupling follow-up

Supplied geometry:

- VDD pin 11 and VSS pin 10 each have a via within 1 mm of the pin.
- VDD connects to In2 3V3; VSS connects to In1 ground.
- In1 is 0.2 mm below the top layer; the supply and ground planes are 1.0 mm apart.
- The 100 nF is 7 mm away and connects to those planes through its own two vias; the 10 nF is 10 mm away.

**Verdict: FAIL for placement approval; move the 100 nF beside pins 10/11.** Short pin-to-plane vias alone do not establish a low-inductance decoupling loop through a remote capacitor. The 1.0 mm plane separation weakens the case for relying on those planes for remote high-frequency decoupling.

AN4938 recommends capacitors as close as possible to MCU power/ground pins and short, wide connections to the planes. It does **not** specify a universal millimetre limit. This verdict is an engineering placement recommendation, not proof that the existing board would malfunction. The capacitor need not literally touch the pins; it needs a short, low-inductance supply-and-return connection near the pair.

Source: [ST AN4938, Rev. 7, section 9.3, Power supply decoupling](https://www.st.com/resource/en/application_note/an4938-getting-started-with-stm32h74xig-and-stm32h75xig-mcu-hardware-development-stmicroelectronics.pdf).

Additional references consulted: [AP63200/AP63201/AP63203/AP63205 datasheet](https://www.diodes.com/datasheet/download/AP63200-AP63201-AP63203-AP63205.pdf), [TI USB layout guidance](https://www.ti.com/video/6087491555001).
