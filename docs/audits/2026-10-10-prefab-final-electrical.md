# Pre-fab final electrical re-check, 2026-10-10

Board and schematics at d267881. Scope is the change set since `2026-10-10-prefab-electrical.md` (c2635eb): K103 on NC contacts, C127 chain, D2 via, R162. Read-only.

## 0. Method

| step | command / source | result |
|---|---|---|
| netlist | `kicad-cli sch export netlist --format kicadsexpr -o /tmp/fe_net.net`, parsed by `/tmp/fe_nets.py` | nets below |
| ERC | `kicad-cli sch erc --severity-all --format json -o /tmp/fe_erc.json` | 0 violations on `/` and `/h7core_block/` |
| DRC + parity | `kicad-cli pcb drc --schematic-parity --severity-all` → `/tmp/fe_drc.json` | 9 lib_footprint_mismatch, 0 unconnected, 81 parity items (5.3) |
| copper | KiCad python `/tmp/fe/b1.py` to `b5.py` | track, via, pad and plane dumps; dangling-end check |
| diff | `git diff c2635eb -- hardware/cubevox.kicad_sch hardware/h7core_block.kicad_sch` | 7 hunks, root sheet only |
| relay pinout | Omron G6K-2F-Y internal connections (p. 6, as cited in the earlier review): 1/8 coil, 2/7 NC, 3/6 COM, 4/5 NO | |

The board carries GND zones on In1.Cu and In2.Cu only. F.Cu has one 5V zone and B.Cu has none, so the brief's "F.Cu and B.Cu GND fill" line is out of date. Pads reach GND through their own vias.

## 1. K103 pad relay

| # | item | evidence | class |
|---|---|---|---|
| 1.1 | Contacts | Netlist: pole A is COM 3 LINE_P, NC 2 IN_P, NO 4 open. Pole B is COM 6 IN_N, NC 7 LINE_N, NO 5 open. R102 22 kΩ sits LINE_P to IN_P and R105 22 kΩ sits LINE_N to IN_N. Coil off closes 2-3 and 6-7, so both pad resistors are shorted and the input is MIC. Coil on opens both, so the pad is in and the input is PEDAL | OK |
| 1.2 | Coil drive | K103.1 is on 5V and K103.8 on PAD_COIL to Q102.3. D110 has its anode on PAD_COIL and its cathode on 5V. Q102.2 is on GND. PAD_EN (U1.41, PE11) drives Q102.1 through R160 1 kΩ, and R161 100 kΩ holds the base to GND. PE11 high turns Q102 on, energises the coil and puts the pad in (PEDAL). PE11 low, or floating at reset or in ROM DFU, leaves Q102 off and the input on MIC | OK |
| 1.3 | IN_P copper | 14 F.Cu tracks of 0.20 mm, 0 vias, 44.76 mm total, 0 dangling ends. The path runs K103.2 (190.70, 86.51) south to y 92.00, east 13.60 mm, north on x 205.30 for 11.48 mm, then to U104.3 (207.60, 78.87), R102.2 (210.75, 79.30), R103.1 (210.75, 77.50) and C120.1 (212.00, 73.17). Every pad is a through-node, so there is no stub. The x 205.30 run sits 0.60 mm centre to centre from A1_FB over 9.07 mm. A1_FB is the inverting input of the same op-amp half, which the earlier review accepted in item 5.4 | OK |
| 1.4 | LINE_N copper | 8 F.Cu and 4 B.Cu tracks, 4 vias of 0.60/0.30 mm, 26.30 mm total, 0 dangling ends, and each via has one F.Cu and one B.Cu track. The order is C125.1 (191.50, 72.16), then a B.Cu hop on x 195.10 (y 70.10 to 74.60), then R105.1 (197.75, 79.49), then the via (197.75, 80.60), then the B.Cu lane to (191.20, 80.60), then the via (190.70, 81.60), then K103.7 (190.70, 79.49) | OK |
| 1.5 | Lane position | The lane on y 80.60 sits 1.11 mm south of the north pad row (y 79.49) and 5.91 mm north of the south row (y 86.51). K103's courtyard spans y 78.15 to 87.86 mm and x 185.96 to 196.65 mm. That puts 5.45 mm of the 6.55 mm lane under the relay body, and the last 1.10 mm runs out to the R105 via | OK |
| 1.6 | Lane coupling | In2 GND lies 0.21 mm under the lane and covers 24 of 27 samples at 0.25 mm pitch. The 3 gaps (x 197.20 to 197.70 mm) are the antipad of the lane's own via. F.Cu above the lane holds only K103 pads 5/6/7 and a LINE_P crossing at x 191.70 mm, and the In1 and In2 planes both separate them from the lane. LINE_P is also the other leg of the same signal. The 5VA B.Cu track (0.30 mm, y 81.40) runs parallel from x 192.40 to 197.75, which is 5.35 mm with a 0.55 mm edge gap. A 0.21 mm plane height bounds the mutual capacitance at about 0.05 pF. In PEDAL the LINE_N node is the pedal's source impedance through C125 10 µF, about 1 kΩ. Even 5 mV of 1.1 MHz buck ripple on 5VA couples about 2 nV, and audio-band 5VA noise couples orders of magnitude less. The coupling is also common to both legs at the INA | OK |
| 1.7 | pins.h | Line 33 reads `kPadEn = PE11; // PAD_EN, output, high = PEDAL (pad in circuit), default low = MIC`. That matches 1.1 and 1.2 | OK |
| 1.8 | Firmware pad handling | `ui/menu.h:15` defaults `inputMic = false`, which means PEDAL, but the spec (line 47) says MIC is the default. No firmware file uses `kPadEn`, so PE11 stays low and the hardware sits on MIC while the menu shows PEDAL. `menu.cpp:20` adds `kMicTrimDb` 25.7 dB in MIC, which was correct for the old fixed pad. With the relay shorting the pad in hardware, that trim double-counts it | FIX (firmware, does not block the production files): drive `kPadEn` high when `!inputMic`, default `inputMic = true`, and set the MIC trim to 0 dB |

## 2. R162 on MUX_S0 (PE12)

| # | item | evidence | class |
|---|---|---|---|
| 2.1 | Net | The schematic net MUX_S0 holds R162.1, U1.42 (PE12) and U101.10 (S0). R162.2 is on /GND. On the board, R162.1 (154.49, 125.22) is the trace corner into U101.10. R162.2 reaches the GND via (156.54, 125.22) by 1.03 mm of 0.30 mm F.Cu. MUX_S0 has 16 tracks, 2 vias and 0 dangling ends | OK |
| 2.2 | GPIO load | R162 is 100 kΩ (C25741, 0402). PE12 driving high pushes 3.3 V / 100 kΩ = 33 µA into it, which is far below the pin's mA-class rating, so the resistor does not fight the push-pull output | OK |
| 2.3 | HC4067 S0 | The resistor loads the GPIO, not the mux. A CMOS select input leaks at most 1 µA, so 100 kΩ holds it under 0.1 V when PE12 floats. That is well under the 3V3A VIL of about 1 V | OK |
| 2.4 | ROM SPI4 sync | In ROM DFU, PE11 NSS sits low through R161 100 kΩ (plus R160), so the slave is selected. SCK on PE12 now holds at 0 V through R162, so it makes no edges. MOSI on PE14 idles high through R128 10 kΩ. A sync byte needs 0x5A shifted in on 8 clock edges. A static SCK gives none, and noise clocks alone would shift in 0xFF. If the ROM enabled an internal pull-up of about 40 kΩ on SCK, the pin would sit static near 2.4 V and still make no edges | OK |
| 2.5 | PE14 ENC_MENU_B (ROM MOSI input) | R128 10 kΩ goes to 3V3, C144 100 nF to GND, and the encoder B contact to GND. Every state stays within 0 to 3.3 V on an input, and turning the encoder without SCK edges shifts nothing | OK |
| 2.6 | PE13 ENC_MENU_SW (ROM MISO output) | R129 10 kΩ goes to 3V3, C145 100 nF to GND, and the encoder switch shorts the net straight to GND. With NSS asserted, the ROM SPI4 slave can drive MISO, and AN2606 does not document its idle level. `menu.cpp:206` fires the click, and with it `platformEnterDfu()`, on the press edge, so the switch can still be closed when the ROM starts. A MISO high into a closed switch is a direct short past the pin's 20 mA rating for as long as the user holds the knob | FIX (firmware): enter DFU on the release edge of the confirm click, after the switch reads open. No board change |

## 3. C127 chain at U105.8

| # | item | evidence | class |
|---|---|---|---|
| 3.1 | Order | The 5VA chain runs from the north trunk (B.Cu x 226.00, y 66.60 to 78.50) to the via (226.00, 78.50), then 0.80 mm of F.Cu to C127.1 (226.80, 78.50). From there 4.82 mm of 0.30 mm F.Cu (226.80, 80.40), (228.30, 81.90) reaches U105.8 (229.10, 81.90). Another 1.30 mm of F.Cu runs to the via (229.10, 80.60), which feeds the south trunk (B.Cu x 229.10, y 80.60 to 94.60). That is the requested order | OK |
| 3.2 | Loop | C127.1 to U105.8 is 4.82 mm of trace and 4.10 mm straight, against 7.3 mm with two vias before the fix. C127.2 goes by 0.80 mm to the GND via (229.00, 78.50) | OK |
| 3.3 | Stubs | 5VA has 60 tracks, 10 vias and 15 pads with 0 dangling ends, and every via has one F.Cu and one B.Cu track | OK |

## 4. D2 USB ESD

| # | item | evidence | class |
|---|---|---|---|
| 4.1 | D2.2 return | D2.2 (114.80, 40.65) runs 0.94 mm of 0.30 mm F.Cu to the GND via (114.80, 41.59, 0.60/0.30 mm). The via drops 0.21 mm to In1, and In1 is solid around it at 0.35 mm radius. The path is about 1.15 mm and about 0.3 nH. At the 8 kV contact peak (30 A, 0.8 ns rise) that adds about 11 V to the USBLC6 clamp. J1's GND pins are 4.6 mm from D2. This is adequate for ESD return | OK |

## 5. Schematic edits

| # | item | evidence | class |
|---|---|---|---|
| 5.1 | Diff | Only `cubevox.kicad_sch` changed: 7 hunks, +117/−6 lines. Hunks 1 to 6 sit at K103 (x 970.28 / 985.52): the two no-connects move to pins 4/5, and the IN_P and LINE_N wires and labels move to pins 2/7. Hunk 7 adds the R162 symbol (100k, 0402, C25741), two wires and the labels MUX_S0 and GND. `h7core_block.kicad_sch` is unchanged | OK |
| 5.2 | ERC | `--severity-all` reports 0 violations on both sheets. The project ignores single_global_label, four_way_junction, simulation_model_issue and footprint_filter | OK |
| 5.3 | Parity | The 81 parity items contain 0 net or missing-part mismatches, so board and schematic nets agree. The rest are 71 missing symbol fields (MPN, Section, Group, Note) and 1 Description text. Three are J101/J106/J108 exclude-from-BOM attributes, which `make_bom.py` handles through its `OWNER_SOLDERED` list. Seven are K103, Q102, D110 and R159 to R161 lacking the `cubevox:` library prefix. None of them changes copper. `gates.sh` reads only violations and unconnected items, so its "parity clean" covers nets, not fields | OK |

## 6. Bench items that travel with the production files

| # | when | check | expect |
|---|---|---|---|
| 1 | upload | JLC placement preview: U107/U108 SOT-23 rotation, C19 tantalum, J117, U1, diodes, polar caps, toggles | pin 1 matches silk |
| 2 | upload | JLC DFM viewer: pot lug slots, J1 0.6 mm slots, 0.50 mm hole gaps at y 56.8 | no DFM flags |
| 3 | upload | part stock (2026-10-02 NOT VERIFIED 9) | all in stock |
| 4 | first power | K103 continuity with the coil off: 2-3 and 6-7 closed, 4/5 open. Coil current in PEDAL | 21 to 28 mA (the earlier review and the spec differ) |
| 5 | first power | K102 muted at reset | OUT_TIP grounded |
| 6 | first power | input current from 9 V and 9.6 V | about 0.19 A |
| 7 | firmware step 0 | silicon revision, ROM bootloader ID (`bringup.py`) | logged |
| 8 | bring-up | HSE start-up time, HSE on MCO, crystal margin | 25.000 MHz |
| 9 | bring-up | SAI1_MCLK_A on PE2 (I01) | `bringup.py` pass |
| 10 | bring-up | DFU by BOOT0 + RESET and from the menu, every toggle in both positions, encoder released | 0483:df11 enumerates |
| 11 | bring-up | scope TP10 VREF_BUF for ringing, VREF after power-up | settled, no ringing |
| 12 | audio | OPA2197 swing, THD, CMRR, noise dBu | spec values |
| 13 | audio | relay click and pop on MUTE and PAD (I03) | inaudible at line out |
| 14 | audio | RVGAIN1 rotation sense | clockwise raises gain |
| 15 | assembly | electrolytic polarity sample, J101 T-pad continuity (NOT VERIFIED 6, 7) | correct |

## Summary

| class | count | items |
|---|---|---|
| FIX | 2 | 1.8 firmware pad default, PE11 drive and MIC trim. 2.6 firmware DFU on click release. Neither blocks the v3 production files |
| DECIDE | 0 | |
| OK | 19 | 1.1 to 1.7, 2.1 to 2.5, 3.1 to 3.3, 4.1, 5.1 to 5.3 |
