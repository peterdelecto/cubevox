# Pre-fab electrical review, 2026-10-10

Board `hardware/cubevox.kicad_pcb` and schematics at c2635eb. This review is read-only. It writes nothing under `hardware/`.

## 0. Method

| step | command / source | result |
|---|---|---|
| ERC | `kicad-cli sch erc --severity-all --format json -o /tmp/pf_erc.json hardware/cubevox.kicad_sch` | 0 errors, 0 warnings, 0 exclusions on both sheets |
| netlist | `kicad-cli sch export netlist --format kicadsexpr -o /tmp/pf_net.net` | 242 parts, 235 nets |
| routing audit | `mega_audit.py hardware/cubevox.kicad_pcb` | parallel 39, viagap 2, slots 5, decap 9, width 6 |
| board geometry | KiCad python scripts `/tmp/pfe/brd.py`, `brd2.py`, `brd3.py`, `brd4.py` | pad-centre distances, track lengths, via drills, hole gaps, slot crossings |
| datasheets | `hardware/datasheets/` (AP63203 covers AP63205, PCM1808, PCM5102A, TLV75533P, CD74HC4067, SP809); Omron G6K p. 6 (omron.com en-g6k.pdf); ST AN2606 / AN4938 bootloader pin list | values cited per item |

ERC skips four checks: single_global_label, four_way_junction, simulation_model_issue and footprint_filter. None of them is electrical. Every distance below is pad centre to pad centre unless stated.

## 1. MCU

| # | item | evidence | class |
|---|---|---|---|
| 1.1 | BOOT0 strap | U1.94 has R2 10 kΩ to GND, and SWBOOT1 pins 3/4 pull it to 3V3. Menu DFU jumps to ROM through `firmware/platform/h7_dfu_reboot.h` | OK |
| 1.2 | ROM bootloader pins on this board | AN2606/AN4938 list SPI4 for the H74x bootloader as NSS PE11, SCK PE12, MISO PE13, MOSI PE14. PE11 is PAD_EN, held low by R160 1 kΩ plus R161 100 kΩ (or Q102's base-emitter junction) to GND, so SPI4 NSS stays asserted. PE12 is MUX_S0 with no pull and floats. In ROM DFU the bootloader therefore listens to a selected SPI4 port whose SCK floats. ST forum reports show noise on unused bootloader inputs blocking interface detection on H743 | FIX: add 100 kΩ pull-downs (C25741, already in BOM) on MUX_S0, S1, S2 and S3 to GND. This also closes 4.6 |
| 1.3 | NRST cap | C14 100 nF sits at (65.82, 86.92) mm, 31.2 mm straight-line from U1.14 (96.50, 91.55). The NRST track is 55.7 mm long and runs to J3.3, SWRESET1 and C14. The pin has no cap | FIX: put a 100 nF within 2 mm of U1.14 by moving C14 or adding one. SWRESET1 needs no cap of its own |
| 1.4 | VCAP1 / VCAP2 | C12 and C13 are 2.2 µF each, 4.67 mm from U1.48 and 2.67 mm from U1.73. The two-VCAP LQFP-100 case needs 2.2 µF each | OK |
| 1.5 | VDDA / VREF+ | FB1 is a 120 Ω ferrite from 3V3. Caps are C8 1 µF, C10 1 µF, C9 100 nF, C11 100 nF and C164 10 µF. C9 is 2.39 mm from U1.21 and 2.64 mm from U1.20 | OK |
| 1.6 | VBAT | U1.6 is on 3V3. C4 100 nF is 2.34 mm away | OK |
| 1.7 | VDD decoupling | U1.11 has C7 at 2.17 mm. U1.27 has C58 10 nF at 2.11 mm and C3 100 nF at 3.39 mm. U1.50 has C61 at 1.55 mm and C6 4.7 µF at 2.85 mm. U1.75 has C57 at 2.64 mm. U1.100 has C56 10 nF at 1.55 mm and C2 100 nF at 2.85 mm. Every VDD pin has a cap within 3.4 mm | OK |
| 1.8 | HSE load | Y1 has CL 12 pF. C15 and C16 are 15 pF, which gives 7.5 pF series plus 3 to 5 pF stray, so 10.5 to 12.5 pF. R1 is 0 Ω | OK |
| 1.9 | HSE traces | HSE_IN is 10.45 mm including the C15 branch. HSE_XTAL_OUT plus HSE_OUT is 6.53 + 4.67 = 11.2 mm through R1. Both are 0.20 mm wide with no vias, and nothing foreign runs under Y1 (`crystal: 0`) | OK |
| 1.10 | SWD | J3 TC2030-NL pinout is 1 VCC, 2 SWDIO, 3 nRESET, 4 SWCLK, 5 GND, 6 SWO. This matches the Tag-Connect Cortex pinout | OK |
| 1.11 | USB 48 MHz | `firmware/cubevox/src/board/sysclock.cpp` sets HSE 25 MHz and PLL1 M5 N192 Q20, which gives 48.000 MHz. `bringup.py` checks it | OK |
| 1.12 | USB series R, ESD | PA11/PA12 run straight to J1. The H7 full-speed PHY needs no series resistors. D2 USBLC6-2SC6 covers both DP and DM, and its pin 5 is on VBUS. C18 is 3.19 mm from D2.5 | OK |
| 1.13 | VBUS sense | R6 33 kΩ over R7 68 kΩ puts 3.37 V on PA9 at 5.00 V and 3.53 V at 5.25 V | OK |
| 1.14 | CC1 / CC2 | R4 and R5 are 5.1 kΩ to GND. That is a UFP sink | OK |
| 1.15 | DP/DM match | Net copper is 59.82 mm for DP and 59.45 mm for DM, so the mismatch is 0.37 mm. Width is 0.20 mm and the pair gap is 0.20 mm over 20.1 mm. 12 Mbit/s full speed tolerates far more | OK |
| 1.16 | Unused MCU pins | 41 U1 pins are on unconnected nets. ERC runs `pin_not_connected` at its default error severity and reports 0, and the H7 sheet has 49 no-connect markers. The pins reset to analog mode | OK |

## 2. Power

| # | item | evidence | class |
|---|---|---|---|
| 2.1 | Reversed plug | J106 has its centre on GND, so the plug is centre-negative. SWPWR1 feeds D106 SS34 in series (40 V). D105 sits after D106. A reversed plug leaves D106 reverse-biased, nothing conducts, and the box stays off | OK |
| 2.2 | Over-voltage, fuse | D105 SMBJ20CA starts conducting at 22.2 V minimum. There is no fuse. A 24 V supply would hold the TVS on and kill it. An 18 V hot-plug through the 177.65 mm VIN_9V run stays under AP63205 VIN absolute max (35 V DC, 40 V for 400 ms) | DECIDE. No fuse: the box runs on 9 V current-limited pedal supplies. PTC fuse: survives a 24 V mistake but adds a part and about 0.1 V drop. Recommend no fuse |
| 2.3 | Buck feedback | AP63205 and AP63203 are fixed-output parts. U103.1 FB connects to 5V and U3.1 FB to 3V3, with no divider | OK |
| 2.4 | Buck parts vs datasheet tables 2/3 | L101 is 4.7 µH and L1 is 3.9 µH, matching the tables. BST caps C116 and C23 are 100 nF. C1 is 10 µF in the table. Fitted are C112 22 µF at 4.55 mm from U103.3 and C20 22 µF at 2.58 mm from U3.3. C2 is 2×22 µF in the table. Fitted are C114 and C115 on 5V plus C19 100 µF, and C21 and C22 on 3V3 | OK |
| 2.5 | C19 tantalum | TAJC107K010 is 100 µF 10 V with 1.2 Ω ESR, on 5V. Derating is exactly 50 % | OK |
| 2.6 | LDO caps | TLV75533P needs CIN ≥ 1 µF and COUT 1 to 200 µF. C24 1 µF is 2.24 mm from U4.1. C25 2.2 µF is 2.20 mm from U4.5. Total nominal 3V3A capacitance is about 37 µF | OK |
| 2.7 | Power-up order | 9 V comes first, then U103 gives 5V, which also brings up 5VA, 5VA_W and the relay supply. U3 EN sits on 5V and gives 3V3. U107 releases LDO_EN 140 / 230 / 560 ms (min / typ / max) after 3V3 passes 2.9 V, and 3V3A follows. R158 100 kΩ pulls EN up. Firmware holds the mux selects low for 1100 ms (`kMuxSelectHoldMs`), which covers the 560 ms maximum | OK |
| 2.8 | Current budget | see table 2a | OK |
| 2.9 | Relay drive | R160 1 kΩ from 3.3 V gives Ib 2.5 mA. The coil draws 21.1 mA at 237 Ω (Omron), so forced β is 8. MMBT3904 is rated 200 mA. D110 has its cathode on 5V and anode on PAD_COIL, which is correct. Coil + on pin 1 to 5V matches the Omron polarity mark. R161 100 kΩ pulls the base down. Q101, R150, R153 and D109 are identical | OK |
| 2.10 | MUTE K102 at reset | PB5 is analog at reset and R153 holds Q101 off. NC 7 then grounds OUT_TIP and NO 4 is open, so the box starts muted | OK |
| 2.11 | PAD K103 at reset | The Omron drawing (G6K-2F-Y internal connections, p. 6) puts NC on pins 2 and 7 and NO on pins 4 and 5. The NC/NO map matches the spec datasheet note. K103 has IN_P on NO 4 and LINE_N on NO 5, so at rest R102/R105 22 kΩ stay in, a −25.8 dB pad. That is safe for line level. Commit c4788e2 and the Astra reply both expect the reverse: shorted at rest (MIC, no coil current). Firmware `menu.h` defaults to PEDAL | FIX: move IN_P to K103.2 and LINE_N to K103.7 (both NC, now unconnected). PAD_EN high then means pad in (PEDAL), as c4788e2 states. Set the firmware default to MIC |

### 2a. Current budget (estimates from datasheet typicals)

| rail | regulator, rating | loads | load |
|---|---|---|---|
| 3V3 | AP63203, 2 A | U1 at 480 MHz ≤ 250 mA (est.), OLED about 20 mA, PCM1808 VDD 5.9 mA, PCM5102A DVDD about 6 mA, LEDs 2 × 2.2 mA, pull-ups about 3 mA | ≤ 290 mA |
| 3V3A | TLV75533P, 500 mA | PCM5102A AVDD + CPVDD about 12 mA, U101 < 1 mA, 16 pots × 0.33 mA | about 19 mA |
| 5VA | FB101 from 5V | U104 + U105 at 4 × 1 mA, U108 1 mA, PCM1808 VCC 8.6 mA typ / 11 mA max, two dividers 0.5 mA | 14 to 17 mA |
| 5VA_W | FB102 from 5V | U106 2 mA, R154/R155 0.25 mA | 2.3 mA |
| 5V | AP63205, 2 A | U3 input about 220 mA, LDO 19 mA, 5VA 17 mA, 5VA_W 2.3 mA, K102 21.1 mA, K103 21.1 mA | about 0.30 A |
| VIN_9V | jack | 0.30 A × 5 V / 0.88 / 9 V | about 0.19 A |

## 3. Analog

| # | item | evidence | class |
|---|---|---|---|
| 3.1 | Gain structure | Stage 1 is 1 + 2 × 4.53 k / 1 k = 10.06, or 20.05 dB. The difference amp is 1.00. Stage 2 is 1 + Ra / (R159 1 k ∥ (Rb + R116 470 Ω)), so 0 to 30.2 dB. Total is 20.0 to 50.2 dB | OK |
| 3.2 | Gain pot law, linear 10 kΩ | At 0 / 25 / 50 / 75 / 90 / 100 % rotation the knob adds 0 / 11.6 / 16.8 / 20.9 / 24.1 / 30.2 dB. The wiper feeds U105.6, a high-impedance input. PRE_OUT's lightest load is 913 Ω at Ra = 0, which is 1.6 mA peak at full scale | OK |
| 3.3 | Headroom, SM58 | ADC full scale is 0.6 × VCC. VCC is about 4.9 V after R20 10 Ω × 8.6 mA, so 2.94 Vpp or 1.47 Vpk. At minimum gain the input clips at 146 mVpk differential, which is −19.7 dBV for a sine. At SM58 −54.5 dBV/Pa that is 128.8 dB SPL. At knob 50 % (36.8 dB total) it is 112 dB SPL. Op-amp outputs swing ±0.73 V around 2.5 V at that level | DECIDE. Keep 20 dB fixed: clips at 129 dB SPL at minimum gain, lowest noise. Drop stage 1 to 14.7 dB (R108/R110 to 2.2 kΩ): clips at 134 dB SPL, more stage 2 noise. Recommend keep |
| 3.4 | 1/4" pad | R102 22 kΩ into 1.19 kΩ (R103 ∥ R144 path) is −25.8 dB. Clip at minimum gain is 2.85 Vpk, or +8.3 dBu. Input impedance is 23.2 kΩ per leg | OK |
| 3.5 | 48 V phantom into the XLR | Through 6.81 kΩ the phantom drives 7.0 mA per leg into the D107/D108 upper diodes to 5VA, 14 mA total. 5VA's own 14 mA load absorbs it. BAT54S is rated 200 mA. MIC_P sits near 5.3 V, so C120/C124 see 2.8 V of reverse bias while phantom is on | DECIDE. Accept: only a male-male cable into a phantom-powered console input causes it, and no part fails short-term. Add a series blocking cap: protects C120/C124 but adds two caps to the mic path. Recommend accept, and say "phantom off" in the manual |
| 3.6 | Clamp at 0 V bias | MIC_P/MIC_N sit at 0 V through R144/R145 100 kΩ. At 128.8 dB SPL each leg peaks at ±73 mV. BAT54 conducts about 0.2 µA at 73 mV, into a source of 47 Ω plus the mic | OK |
| 3.7 | VREF_BUF stability | U108 OPA197 is a follower with its feedback taken at its own output. R146 22 Ω isolates C160 10 µF. The zero sits at 723 Hz, well under the 10 MHz GBW, so the load looks like 22 Ω plus ESR at crossover. R117/R118 10 kΩ and C135 10 µF set a 3.2 Hz corner | OK |
| 3.8 | ADC anti-alias, DC block | R119 100 Ω and C136 2.2 nF give 723 kHz. PCM1808's internal filter is −3 dB at 1.3 MHz. C137 10 µF into 60 kΩ is 0.27 Hz. VINR goes to GND through C138 | OK |
| 3.9 | Input and gain corners | C120/C124 47 µF into about 1.25 kΩ is 2.7 Hz. C158 22 µF with R109 1 kΩ is 7.2 Hz. C159 100 µF with 320 Ω at maximum gain is 5.0 Hz | OK |
| 3.10 | DAC output | R147 470 Ω with C161 2.2 nF is 154 kHz. The DAC sees about 25 kΩ (minimum 1 kΩ). The 10 k / 15 k divider gives × 0.6, so 1.26 Vrms or +4.2 dBu maximum | OK |
| 3.11 | Output drive | U106B drives R124 100 Ω and C141 10 µF. Into 600 Ω: 2.5 mA peak, HPF 22.7 Hz. Into 10 kΩ: HPF 1.5 Hz. Into 32 Ω headphones (not a target): 13.5 mA peak, −12.3 dB, HPF 121 Hz | OK |
| 3.12 | Pot mux | 16 pots go to 16 channels of U101 (I0 to I15). KEY and SEMITONES are slot pots. τ = (2.5 kΩ wiper + RON ≤ 180 Ω + R126 1 kΩ) × C142 10 nF ≈ 37 µs. 16-bit settling takes 11 τ, or 0.41 ms, against PINMAP's 0.5 ms wait. A 16-channel scan takes 8 ms, so each pot updates at 125 Hz | OK |
| 3.13 | ADC reference | VREF+ is VDDA, from the 3V3 buck through FB1. The pots run on 3V3A. That leaves a few % ratiometric error, accepted 2026-10-08 | OK |
| 3.14 | Stepped pots | SEMITONES has 25 positions, so 164 counts per step at 12 bits. Firmware hysteresis covers it | OK |

## 4. Digital

| # | item | evidence | class |
|---|---|---|---|
| 4.1 | I2C | R142 and R143 are 4.7 kΩ to 3V3. SCL is 76.67 mm and SDA 64.95 mm. Bus capacitance is about 30 pF with the module, so tr ≈ 0.12 µs, under the 300 ns limit at 400 kHz | OK |
| 4.2 | SAI clocks | SAI1_MCLK_A on PE2 drives R27 33 Ω 2.9 mm from the pin, then 50.21 mm with 2 vias to U8.6. BCLK goes through R29 33 Ω, 53.34 mm with 2 vias. PCM5102A SCK is grounded, so its PLL runs from BCK | OK |
| 4.3 | OLED | J117.2 is on 3V3 and J117.1 on GND. The 4-pin module resets itself | OK |
| 4.4 | Encoder | R127 to R129 are 10 kΩ pull-ups with C143 to C145 100 nF, so τ is 1.0 ms. The caps sit 3.6 mm from ENC101 | OK |
| 4.5 | Toggles | R134 to R141 are 10 kΩ to 3V3, and C150 to C157 are 100 nF placed 8.1 mm from each switch. A closed switch reads low. An HBM 100 pF / 8 kV hit into 100 nF rises about 8 V. MUTE_SW has R120/C162 2.3 mm from U1.63 | OK |
| 4.6 | Floating logic inputs | MUX_S0 to S3 float at reset and in ROM DFU into the HC4067 CMOS inputs on 3V3A. The SAI lines float until init, but XSMT R16 10 kΩ holds the DAC muted | covered by the 1.2 FIX |
| 4.7 | LEDs | D1 on PB7 through R3 680 Ω draws 2.2 mA. D3 through R10 680 Ω also draws 2.2 mA | OK |

## 5. Layout items with electrical meaning

The board is 4-layer (`GetCopperLayerCount()` = 4). In1.Cu and In2.Cu are full GND zones 0.21 mm under F.Cu and 0.21 mm over B.Cu. Every outer trace therefore has a solid return plane 0.21 mm away. Coupling between neighbours is far below the 2-layer case, because the gap to the plane is half the 0.40 mm trace spacing or less.

| # | item | evidence | class |
|---|---|---|---|
| 5.1 | VA_SENSE beside PAD_EN, B.Cu, 0.40 mm gap over 38.9 mm, In2 GND 0.21 mm below | Both are DC. PAD_EN changes only from the menu. Even a 2-layer coupling bound of 2 pF into C163 100 nF gives 3.3 V × 2 p / 100 n = 66 µV, and the plane cuts that further | OK |
| 5.2 | VBUS_SENSE beside USB_DM, 0.60 mm over 7.8 mm and 0.80 mm over 4.0 mm | VBUS_SENSE is a 3.37 V DC node with a 22 kΩ Thevenin source. Edge spikes stay under 0.2 V, so PA9 stays above VIH | OK |
| 5.3 | XSMT beside LDOO, 0.39 mm over 3.3 mm | One is a DC control and the other the DAC's internal LDO node with C33 | OK |
| 5.4 | Analog pairs | POT pairs run 0.25 to 0.50 mm over up to 37.5 mm, but they carry DC wipers read one at a time. INA_O1 and INA_O2 are two legs of one differential signal. IN_P and A1_FB are the two inputs of one op-amp at virtual short. XLR_3 and TRS_R share one jack, used one plug at a time | OK |
| 5.5 | Tight 0.20 mm runs | The TOGGLE bus is DC. TOGGLE2/TOGGLE3 beside XSMT: XSMT is push-pull driven, and at reset R16 10 kΩ limits a glitch to about 0.5 V for about 20 ns, under VIH 2.3 V. The USB pair runs tight by design | OK |
| 5.6 | viagap | GND/VIN_9V at (230.39, 53.30) and GND/5V at (107.33, 56.80) are 0.80 mm apart with 0.30 mm drills, so the hole gap is 0.50 mm. That equals JLC's 0.50 mm. A full scan finds the minimum different-net hole gap is 0.500 mm on 3 pairs, then PAD_EN to MUX_S0 at 0.506 mm | OK |
| 5.7 | DRC hole rule | Board setup has hole_to_hole 0.25 mm against JLC's 0.50 mm, so DRC would pass a 0.30 mm gap | FIX: set board hole-to-hole to 0.50 mm |
| 5.8 | slots | Five via clusters perforate In1 and In2: POT09 to POT16 at (145.6 to 150.4, 125.9 to 130.4) and (145.6 to 150.4, 153.1 to 158.0), MUX_S0 to S3 plus PAD_EN at (114.1 to 115.9, 82.5 to 84.7), ADC_DOUT/GND/BCLK at (118.7 to 119.7, 99.6 to 101.2), and ENC at (100.3 to 101.6, 80.5 to 81.8). Each plane stays continuous around the holes. No foreign F.Cu or B.Cu track crosses any cluster's box plus 0.6 mm | OK |
| 5.8b | Drill to inner GND copper | The smallest gap from a non-GND via drill edge to the filled In1/In2 GND copper is 0.40 mm (NRST at (65.82, 84.60), POT12 at (147.64, 128.48), 3V3 at (87.25, 74.48)). The limit is 0.20 mm. Script `/tmp/pfe/brd5.py` | OK |
| 5.9 | width | 3V3 is 0.30 mm with two 0.40 mm segments, carrying ≤ 290 mA. The 5V branches are 0.30/0.40/0.50 mm, and the 0.30 mm one is the 21 mA coil branch. 9V_JACK and 9V_PLUG are 0.40/0.50 mm at 0.19 A. VBUS is 0.20/0.30 mm and carries µA. VCAP1 has a 0.30 mm neck | OK |
| 5.10 | Board stack | `GetCopperLayerCount()` returns 4, matching the brief's JLC04161H-7628 4-layer stackup | OK |
| 5.11 | Production files | `production/` holds only v2 files. `cubevox-cpl-v2.csv` dates from 2026-10-09 (2093e6a) and has 0 rows for K103, Q102 and R160. The board changed after that in 560d966 and c2635eb | FIX: generate the v3 gerbers, drill, BOM and CPL from the board after the FIX items land. Do not upload v2 |

### 5a. Decap distances

A cap whose GND pad reaches the inner plane through a nearby via is normal on this stackup. The two distances that matter are IC pin to cap pad and cap GND pad to its GND via. Script `/tmp/pfe/brd5.py` measured them pad centre to pad or via centre. Every IC decoupler's GND pad sits within 1.25 mm of a GND via, and every IC decoupler is within 4.67 mm of its pin.

| cap | IC pin | pin to cap (mm) | cap GND pad to GND via (mm) | class |
|---|---|---|---|---|
| C4 / C7 | U1.6 / U1.11 | 2.34 / 2.17 | 0.88 / 0.77 | OK |
| C58, C3 | U1.27 | 2.11, 3.39 | 0.79, 0.80 | OK |
| C61, C6 | U1.50 | 1.55, 2.85 | 0.84, 0.85 | OK |
| C57 | U1.75 | 2.64 | 0.68 | OK |
| C56, C2 | U1.100 | 1.55, 2.85 | 0.79, 0.80 | OK |
| C9 | U1.21 VDDA | 2.39 | 0.85 | OK |
| C12 / C13 | U1.48 / U1.73 VCAP | 4.67 / 2.67 | 0.77 / 0.92 | OK |
| C165, C111, C101, C170 | U6.20, U6.1, U6.8, U6.1 | 1.79, 2.74, 2.43, 3.97 | 1.03, 0.75, 0.95, 1.25 | OK |
| C45, C49, C47 | U8.3, U8.1, U8.4 | 2.47, 2.64, 2.62 | 0.78, 0.85, 0.85 | OK |
| C27 | U101.24 | 2.14 | 0.85 | OK |
| C118 / C127 | U104.8 / U105.8 | 1.90 / 4.10 | 0.80 / 0.80 | OK. C127's supply side reaches U105.8 through vias at (229.10, 80.60) and (226.00, 78.50), with its power pad 0.80 mm from its via. At 4.1 mm it is closer than VCAP1's 4.67 mm |
| C130, C167, C166 | U106.8, U108.5, U107.3 | 1.80, 1.48, 2.22 | 1.20, 1.12, 0.80 | OK |
| C24 / C25 | U4.1 / U4.5 | 2.24 / 2.20 | 0.80 / 0.85 | OK |
| C113, C112 | U103.3 | 2.05, 4.55 | 0.80, 1.32 | OK |
| C20 | U3.3 | 2.58 | 0.98 | OK |

The mega audit's nine via-fed caps:

| ref | net, value | position (mm) | role | GND pad to GND via (mm) | class |
|---|---|---|---|---|---|
| C18 | VBUS 1 µF | (111.91, 39.70) | connector bulk, 3.19 mm from D2.5 | 0.88 | OK |
| C21 / C22 | 3V3 22 µF | (85.85 / 88.60, 67.80) | buck output, 5.7 / 2.9 mm from L1.2 on F.Cu | 0.97 / 0.97 | OK |
| C29 / C30 | 3V3A 100 nF / 10 µF | (164.22, 102.40 / 100.30) | rail bulk. U101.24's decoupler is C27 | 1.00 / 1.00 | OK |
| C127 | 5VA 100 nF | (226.80, 78.50) | U105.8 decoupler, see the row above | 0.80 | OK |
| C134 | 5VA 100 nF | (133.50, 97.17) | input of the R20 / ADC_VCC filter | 0.85 | OK |
| C160 | VREF 10 µF | (155.84, 97.00) | VREF node cap. No IC pin is on VREF | 1.84 | OK |
| C117 | 5VA 10 µF | (157.60, 67.19) | FB101 output bulk, 5.4 mm from FB101.2 | 1.60 | OK |

### 5b. Docs and firmware

| # | item | evidence | class |
|---|---|---|---|
| 5.12 | PE11 | PINMAP.md line 53 still calls PE11 "spare". `firmware/cubevox/src/board/pins.h` line 54 lists PE11 as unconnected, but the netlist puts PAD_EN on U1.41 | FIX: add PAD_EN (PE11, high = PEDAL after 2.11) to both |
| 5.13 | PARTS.md | There are no rows for K103, Q102, D110, R159, R160 or R161. Count line 3 says "1 relay". Row 20c says RVGAIN1 is C470307 log, but the schematic has C361173 linear with the divider law | FIX: update rows 13, 20c, 32, 63 and the counts |

## 6. Close-out of earlier audits

| source | item | status |
|---|---|---|
| 2026-10-02 response | A01 to A09 | closed. The netlist shows J101.6/7 both on TRS_T, OPA2197 on U104 to U106, the U108 follower, C158/C159, R111 to R114 at 0.1 %, and R147/C161 |
| 2026-10-02 | I01, PE2 SAI1_MCLK_A | open on the bench (`bringup.py` checks the SAI1 clock) |
| 2026-10-02 | I02, 0.5 ms settle | closed, PINMAP.md line 268 |
| 2026-10-02 | I03, relay click | closed in spec item 7 (XSMT before MUTE_N). Listening check still open |
| 2026-10-02 | I04 | BOM export closed by `make_bom.py` but stale, see 5.11. PARTS SWD text closed (WARNINGS 4). Separate analog sheets still open, cosmetic |
| 2026-10-02 | NOT VERIFIED 1, VREF stability | analysis done (3.7). Scope TP10 on the bench |
| 2026-10-02 | NOT VERIFIED 2 to 4, 6, 7 (THD, CMRR, relay pop, electrolytic polarity sample, J101 T-pad continuity) | open on the bench |
| 2026-10-02 | NOT VERIFIED 8, DRC | closed. `gates.sh` ALL PASS at c2635eb (brief) |
| 2026-10-02 | NOT VERIFIED 9, stock | open until order time |
| 2026-10-06 round 5 | U101 select back-power | closed by firmware (`kMuxSelectHoldMs` 1100 ms) |
| 2026-10-06 | POT/TOGGLE unrouted | closed, 0 unconnected |
| 2026-10-06 | C18 value | closed, board value 1uF |
| 2026-10-06 | J117 CPL | closed, CPL y −57.750 |
| 2026-10-06 | L101 attribute | closed, FP_SMD |
| 2026-10-06 | Fiducials | closed, FID1 to FID3 with windows |
| 2026-10-06 | H101 to H104 keepouts | closed, m3seat rule areas |
| 2026-10-06 | VA_SENSE stitching | accepted. GND vias are 4.51, 4.37 and 2.46 mm from its three vias, and C163 limits its bandwidth |
| 2026-10-06 | USB stackup | closed, not needed at full speed (1.15) |
| 2026-10-06 | USER_LED pin | closed, PB7 in the netlist and PINMAP line 75 |
| 2026-10-06 | DRC rerun | closed, `gates.sh` ALL PASS |
| 2026-10-07 Astra | F1, F3 to F8 | closed in 93d650b. Re-checked: F3 at PINMAP line 268, F4 at PARTS row 70, F5 at PINMAP line 254 against spec F4, F7 at PARTS row 28b (9 mm), F8 at PARTS row 69 |
| 2026-10-07 Astra | F2, PARTS counts | reopened by K103, see 5.13 |
| 2026-10-08 review | changes 1 to 8 | closed. D105 is SMBJ20CA C113992. R158 is 100 kΩ. C170 sits 3.97 mm from U6.1. R7 is 68 kΩ. TP1 to TP5 are present. Hole-to-copper is 0.30 mm. The spec F3 line 188 has PC5 |
| 2026-10-08 | change 9, gain taper | superseded by the c4788e2 divider law (3.2). PARTS row 20c stale, see 5.13 |
| 2026-10-08 | firmware step 0, items 1 to 3 | closed. `sysclock.cpp`, the mux hold and the USB soft-connect are in the firmware README |
| 2026-10-08 | step 0, item 4 (silicon revision, bootloader ID) | open, first board |
| 2026-10-08 | residual risks 1 to 4 (crystal margin gm_crit 1.11 against 1.5 mA/V, 0.2 A input, DFU entry, JLC DFM) | open on the bench. DFU entry also depends on 1.2 |
| 2026-10-09 mega audit | fix list 1 to 26 | closed per its section 7, gates ALL PASS |
| 2026-10-09 | not checked 3 (OPA2197 swing, noise, pot rotation) | swing closed (3.3). Noise and RVGAIN1 rotation sense still open on the bench |

## 7. Bench items carried forward

1. DFU by BOOT0 + RESET and from the menu, tried with every toggle in both positions after the 1.2 fix.
2. HSE start-up time at power-up, plus the HSE frequency on MCO.
3. Scope TP10 (VREF_BUF) for ringing, and VREF after power-up.
4. K103 rest state with a 1/4" SM58 after the 2.11 fix. Expect no pad at rest and 21 mA in PEDAL.
5. Input current from 9 V. Expect about 0.19 A.

## Summary

| class | count | items |
|---|---|---|
| FIX | 7 | 1.2 mux select pull-downs (bootloader SPI4 SCK and HC4067 inputs), 1.3 NRST cap at the pin, 2.11 K103 contacts against the rest-is-MIC intent, 5.7 hole-to-hole rule, 5.11 generate v3 production files, 5.12 PE11 in PINMAP and pins.h, 5.13 PARTS.md rows |
| DECIDE | 3 | 2.2 no fuse (recommend none), 3.3 stage 1 gain (recommend keep 20 dB), 3.5 phantom (recommend accept) |
| OK | 60 | 14 MCU, 9 power, 12 analog, 6 digital, 10 layout (5.1 to 5.6, 5.8, 5.8b, 5.9, 5.10), 9 via-fed caps (5a) |
