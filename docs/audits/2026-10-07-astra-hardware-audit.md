# Independent KiCad 9 hardware audit — cubevox — 2026-10-07
## progress: read design intent, pin map, parts, and recent run log
## progress: run ERC and DRC including schematic parity; export connectivity
### F1 [SHOULD-FIX] OLED symbol differs from project library
- Where: J117, root schematic at (7.9756, 5.969) mm.
- What: Fresh ERC reports `lib_symbol_mismatch`: "Symbol 'OLED-1.3-SH1106-I2C-4P' doesn't match copy in library 'cubevox'". No electrical ERC errors were reported; the significance of the symbol difference needs comparison.
- Fix: Reconcile the embedded symbol and library definition, preserving the intended socket pin order and footprint.

## progress: investigate DRC process abort and installed KiCad version
### F2 [SHOULD-FIX] Parts ordering summary still describes the old panel
- Where: hardware/PARTS.md, introductory build quantity, Counts item 3, and Dropped from fxbox.
- What: The summary says `15 pots, 3 encoders ... 2 relays`, while current rows 20/18/13 specify 17 pots, one encoder and one relay; the dropped list also includes PJ-611E C309282 although row 22 requires it. These contradictory purchasing instructions survived the slots revision.
- Fix: Update the ordering summary to 17 pots, one MENU encoder and K102 only, and remove the required PJ-611E from the dropped list.
Audit limitation: requested DRC command exited 134 (SIGABRT) before writing drc.json. Installed CLI is 10.0.5, not 9; crash report shows macOS `_RegisterApplication` / `wxGetMousePosition` initialization. Fresh DRC/parity results are therefore unavailable; historical RUNLOG passes are not treated as fresh verification.
## progress: schematic power, codec straps, MCU interfaces, controls, and analog protection
### F3 [SHOULD-FIX] PINMAP settling instruction omits the dominant mux RC
- Where: hardware/PINMAP.md Firmware item 8; R126/C142 ahead of R30/C55, U101 to U1 PC0.
- What: PINMAP says to wait `about 1 us` after selecting a channel, but netlist extraction shows R126=1k, C142=10nF before the documented 100R/1nF stage. The first RC alone is 10 us, increasing to roughly 35–40 us near the midpoint of a 10k pot, so 1 us cannot settle a full-scale channel change; the requested 0.5 ms firmware assumption is consistent with this network.
- Fix: Replace the 1 us instruction with the design's minimum 0.5 ms channel settling time and retain the separate ADC acquisition-time requirement.
### F4 [SHOULD-FIX] Analog supply and reference documentation misses the implemented revision
- Where: hardware/PARTS.md row 50; hardware design spec Decisions 5, 8, 13; U108, U106, FB102, R154/R155.
- What: The docs assign VREF buffering to U106 and describe its supply as shared 5VA, but the exported netlist has a separate OPA197IDBVR U108 driving VREF_BUF, U106 on 5VA_W through FB102, and the DAC divider returning to BIAS_W (R154/R155), not VREF. U108 has no entry in PARTS.md.
- Fix: Document and include U108 and the implemented 5VA_W/BIAS_W network in the parts list and electrical intent.
## progress: PCB routing, power widths, return planes, decoupling, crystal and USB geometry
Independent netlist-to-PCB extraction: no pad-net or component-value differences. Codec strap nets, SAI/MCU assignments, 16 pot polarities, nine toggle RC networks, MENU RC, relay flyback/base/pulldown and BOOT0/NRST connectivity checked. U4 takes 5V with 1uF input/2.2uF output; both MCU VCAP nodes have 2.2uF. No additional floating functional input or output-output connection found in the exported netlist.
### F5 [SHOULD-FIX] Conflicting PLL register values in firmware guidance
- Where: hardware/PINMAP.md:229 versus hardware design spec F4.
- What: PINMAP specifies `DIVN3 = 196` and `DIVP3 = 4`, whereas F4 specifies register values `DIVN3 = 195, DIVP3 = 3` for the same M/N/P ratios 25/196/4. The distinction between divider ratios and encoded register fields is not consistent, so the two instructions cannot both be copied into register initialization.
- Fix: Make PINMAP use the same encoded register values as F4 and explicitly distinguish ratios from register fields.
### F6 [NIT] Pin-change table still marks active toggle GPIOs unused
- Where: hardware/PINMAP.md:211-212, section 5 Pin changes.
- What: The cubevox-net column marks PB2 and PE7–PE10 `free (NC)`, but the current netlist and section 2 use PB2=TOGGLE6, PE7=TOGGLE7, PE8=TOGGLE8 and PE9=XSMT. Only PE10 is free in that range.
- Fix: Correct the section 5 table or explicitly label it as a superseded intermediate mapping.
### F7 [SHOULD-FIX] OLED standoff purchase instruction contradicts the mechanical spec
- Where: hardware/PARTS.md row 28b versus hardware design spec items 22 and 27; J117.
- What: PARTS instructs `Buy 4x M3 x 8 mm standoffs` and describes an 8 mm model, while the current mechanical spec calls for 9 mm standoffs and bases socket engagement on that height. Actual lid/module fit cannot be signed off from these contradictory dimensions.
- Fix: Reconcile the purchase length and model description with the intended 9 mm OLED mounting stack.
## progress: filled-plane return coverage, panel spacing, thermal relief and assembly side
PCB connectivity API reports 34 unconnected items. Only J101/J106/J108 are on B.Cu; these are explicitly designated owner-soldered in the supplied spec and PARTS, despite the prompt's all-F.Cu shorthand. No unintended bottom-side component was found. Both inner layers contain GND zones with thermal connections; pot ground pads inherit those settings. H101–H104 have outer-layer m3seat rule areas.
## progress: consistency cross-check and remaining layout evidence
The PCB API DRC fallback also failed (exit 139), so it does not close the fresh DRC limitation. Filled-plane sampling at 0.2 mm found continuous reference copper under I2C and USB; SAI gaps are being checked against normal signal-via antipads. USB trace lengths are DP 59.29 mm and DM 58.92 mm at 0.2 mm width. SAI/I2C pin assignments match PINMAP section 2 and ST DS12110; PCM1808 and PCM5102A straps match TI datasheets.
F1 clarification: direct symbol diff shows identical pins and geometry; the library's `LCSC Part` is `owner-supplied module, hand-plugged`, while the embedded symbol correctly says `C42379197`. This is procurement/library metadata drift, not an electrical pin-order error; update the library to the assembled socket identity.
### F8 [SHOULD-FIX] LDO sequencing supervisor is absent from the parts list
- Where: U107 in h7core_block.kicad_sch; hardware/PARTS.md.
- What: The exported netlist includes `U107 SP809EK-L-2-9/TR`, powered from 3V3 and driving U4 EN through LDO_EN, but PARTS.md has no SP809 or U107 entry. This functional IC is required for the startup sequence described in PINMAP and is omitted from the maintained procurement list.
- Fix: Add U107 with its actual ordering code, footprint and quantity to PARTS.md.
## progress: finalize evidence, limitations and finding counts
Additional checks: 16 slot pots RV101–RV116, eight SW101–SW108, SWBYPASS1 on MUTE_SW, ENC101 only and K102 only are present; K101/D104/ENC102/ENC103 are absent. Every schematic footprint name, component value and exported pin-net association matches the PCB. Panel courtyard collision checks returned none, and visible reference text bounding boxes did not intersect pad boxes; visible reference text is 1 mm high.

Layout assessment: 5V traces are 0.4/0.5 mm, 3V3 mostly 0.3 mm, relay coil 0.3 mm for the approximately 21 mA coil; no definite current-capacity defect established. The input preamp is at x205–231 versus the MCU/bucks at x94–96. All inspected IC supply pins have bypass capacitors; notably U1 VCAP1 to C12 is 4.67 mm straight-line and U6 flying-cap pins to C34 are about 6.14 mm, so this is not a measurement of their transient performance. The 25 MHz crystal has 15 pF load caps (7.5 pF series equivalent plus unknown board/pin parasitics); startup margin and exact load cannot be proved statically. BCLK passes below the oscillator area on B.Cu at y100.55, with two intervening ground planes; this alone is not established as a defect. Plane sampling found no SAI gaps beyond 0.7 mm of signal-via centers, consistent with antipads rather than a plane split.

Electrical assessment: preamp resistors give stage-one gain 10.06 and stage-two gain 1–31.3, consistent with approximately 20–50 dB total. Input clamp orientation, coupling polarity, gain-pot wiper tie, grounded unused ADC input through its coupling cap, DAC strap levels, XSMT pulldown, and relay reset-to-muted wiring agree with intent. The supplied documentation describes 9V barrel input followed by a 5V buck, rather than direct 5V input; this was treated as the circuit's intent.

Sources checked: [TI PCM1808](https://www.ti.com/lit/ds/symlink/pcm1808.pdf), [TI PCM5102A](https://www.ti.com/lit/ds/symlink/pcm5102a.pdf), [TI TLV755P](https://www.ti.com/lit/ds/symlink/tlv755p.pdf), and [ST DS12110](https://www.st.com/resource/en/datasheet/stm32h742xi.pdf) support the strap, supply and alternate-function checks above.

Limitations: no fresh full DRC report could be generated, so clearance/short-circuit compliance is not certified. Connectivity count and independent parity checks do not replace DRC. No matching lid solid was located for a 3D collision check; panel footprint courtyards and documented heights were checked, not actual enclosure fit. USB impedance and analog noise/startup performance were not simulated or measured. Accepted unrouted items and footprint warnings are not findings.

## done
BLOCKING: 0 confirmed (subject to the stated verification limitations).
SHOULD-FIX: 7.
NIT: 1.
