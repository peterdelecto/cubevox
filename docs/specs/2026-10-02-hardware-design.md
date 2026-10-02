# cubevox hardware design

Owner interview 2026-10-02. This spec is the design brief for the KiCad
schematic. Parts are LCSC codes from the 2026-10-02 search; stock and pin
maps are re-verified live before the schematic is drawn (ClaudeRouter rule:
a part without a code is not a part).

## Purpose line (owner confirmed 2026-10-02)

cubevox: a tabletop vocal effects board for Adam Keith. One SM58 or pedal in
on a combo jack, through an analog preamp into an STM32H743 at 48 kHz, out a
buffered 1/4" jack, with a relay bypass that keeps the preamp path alive when
the digital is off. 9 V Boss supply, USB-C for firmware only, 15 pots, 3
encoders, toggles and an OLED on one JLC-assembled board inside a printed box.

## Plan

1. Clone the Teensy-H7-Port fxbox / h7core design into `cubevox/hardware/`
   (read-only source; copying out alters nothing in it). Keep its MCU,
   crystal, USB-C DFU, SWD, QSPI flash, ADC, DAC, 3V3 power and 9 V front end.
2. Replace the fxbox audio front end with a mic/line preamp fed by one
   XLR + 1/4" combo jack.
3. Replace the fxbox panel with cubevox's controls (15 pots on a mux, KEY
   encoder, toggles, OLED header).
4. Follow the ClaudeRouter board recipe in order: purpose line → brief →
   PARTS.md with codes → `foreman schematic` from a JSON spec → ERC 0 +
   visual render → layout.

## Open questions resolved

| # | Question | Answer |
|---|----------|--------|
| 1 | Digital core | Clone fxbox/h7core |
| 2 | Input connector | Neutrik NCJ6FA-H combo (C368458, THT) because LCSC stocks it; separate XLR + 1/4" only if it drops out of stock |
| 3 | Output | Buffered 1/4" TS, instrument/line level set in a firmware menu |
| 4 | Bypass | Relay bypass after the preamp; digital path skipped |
| 5 | Preamp | Discrete instrumentation amp from a dual low-noise op-amp (NE5532 / OPA1678 class) |
| 6 | Input gain | Analog pot in the preamp gain leg; firmware never sees it |
| 7 | Analog supply | Single 5 V rail with mid-rail bias, shared with the ADC |
| 8 | Display | 1.3" SH1106 OLED, I2C, 4-pin header |
| 9 | Per-effect on/off | Toggle switches; autotune for certain, the rest pending Adam. GPIO held for 8 |
| 10 | Bypass actuator | Panel toggle, same switch part as 9. No footswitch (tabletop, printed box) |
| 11 | Board arrangement | One board; controls up through the top, jacks right-angle out the rear |
| 12 | 1/4" half of the combo | Mic or pedal, never both inputs at once. No pad; shared 0–60 dB preamp, ~20 kΩ input |
| 13 | Which H7 | Bare STM32H743VIT6 soldered on the product board, as fxbox. Not the WeAct module |
| 14 | Top face | Signal-flow row like the pedal view, about 230 × 110 mm, provisional. Arrangement is decided at the PCB step, not now |
| 15 | Gain structure | Two stages, 20 dB fixed + 0–40 dB on the pot. 1/4" contacts carry a 20 dB pad; menu item 1/4" INPUT: PEDAL / MIC adds 20 dB in firmware for a mic on a TRS cable; the combo's normalling contact tells the MCU a 1/4" plug is in |
| 16 | Power ground | Centre pin straight to GND, +9 V sleeve through a series Schottky and TVS. fxbox's either-polarity FET bridge is dropped because the Boss daisy chain and the audio cable share ground with the BD-2 |
| 17 | Toggle switch | Yuen Fung ST-0-102-A01-T000-LF (C1788487), the only ON-ON bat toggle on JLC whose bushing spans the 11.8 mm panel plane; lever tip 3.6 mm above the knobs |

## Decisions

### Signal chain

1. Combo jack NCJ6FA-H. XLR pin 2 → MIC_P, pin 3 → MIC_N, pin 1 and G to
   GND at the jack. TRS T → LINE_P, R → LINE_N through the 20 dB pad into the
   same preamp nodes, S to GND. The second T hole is the normalling contact;
   it goes to an MCU GPIO (JACK_TRS_N, pulled up, low when a plug is in).
   A TS plug shorts ring to sleeve, which grounds the cold input.
2. Input network, per leg (P and N identical). XLR leg: 47 Ω series,
   BAT54S clamps to 5V/GND after it, 100 pF to GND, 10 µF DC block into
   node IN_x. TRS leg: 10 µF DC block, 10 kΩ series into node IN_x. Node IN_x:
   1.2 kΩ to VREF. The 1.2 kΩ is both the mic bias path and the pad shunt:
   pad = 1.2/(10+1.2) = −19.4 dB (firmware MIC trim is +19.4 dB), mic input
   impedance 2.4 kΩ differential, pedal input impedance about 11 kΩ.
3. Stage 1, fixed 20 dB. Instrumentation amp on OPA1678IDR (C192421): A1 and
   A2 non-inverting with Rf 4.7 kΩ each and RG 1 kΩ between inverting inputs,
   G = 1 + 2·4.7k/1k = 10.4 (20.3 dB). Difference amp A3 with four 10 kΩ, gain
   1, reference VREF. 47 pF across each Rf. Output PRE1.
4. Stage 2, 0–40 dB on the Input Gain pot (RK09D1130C2P 10 kΩ linear, wired
   as a variable resistor, wiper tied to one end). Non-inverting on A4:
   feedback = pot + 100 Ω end-stop, 100 Ω from inverting input to VREF,
   47 pF across the feedback. G = 1 + (Rpot + 100)/100 = 2 .. 102, so the
   stage runs 6 to 40 dB and the chain 26 to 60 dB on XLR, 6.6 to 40.6 dB on
   the 1/4". Output PRE_OUT, DC at VREF. Max swing about 1.4 Vrms on 5 V.
5. VREF. 5V through 10 kΩ / 10 kΩ, 10 µF, buffered by one OPA1678 half, 100 nF
   at each load. Three OPA1678 duals in total (A1–A4, buffer, VREF).
6. ADC feed. PRE_OUT → 100 Ω → 10 µF → PCM1808 VINL, 2.2 nF after the 100 Ω
   (anti-alias corner ~720 kHz is fine with the ADC's oversampling). ADC is
   always fed, so firmware can meter in bypass. VINR follows fxbox's unused-
   channel treatment.
7. Bypass relay G6K-2F-Y DPDT 5 V (C47190). Both poles wired in parallel for
   contact reliability: COM (3, 6) = BUF_IN, NC (2, 7) = PRE_OUT, NO (4, 5) =
   DAC_ATT. Coil + (1) from 5V through the bypass toggle, coil − (8) to GND,
   1N4148W (C81598) flyback across the coil. Coil energised = effect on.
   FX_ON_SENSE: coil + node through 10 kΩ / 18 kΩ to GND into an MCU GPIO
   (3.2 V when on). No MCU in the switching path.
8. DAC. PCM5102APWR (C107671) as fxbox, output ground-centred ±3 V. DAC_ATT:
   OUTL → 10 µF → 10 kΩ / 15 kΩ divider to VREF (×0.6, 1.27 Vrms max) so the
   single-rail buffer never clips. Firmware LINE / INSTRUMENT menu scales
   digitally below that.
9. Output buffer. A5 (OPA1678) unity, input BUF_IN, output → 100 Ω → 10 µF →
   100 kΩ to GND → OUT_TIP. Jack: NMJ6HCD2 (C368502) T to OUT_TIP, S to GND,
   switch contacts no-connect. PJ-611E is dropped.
10. ADC/DAC clocks as fxbox: SAI1 BCLK PE5 (33 Ω), LRCLK PE4, DAC SD PE6,
    ADC SD PE3, ADC SCKI PD14 at 12.288 MHz for 48 kHz (PLL3 recomputed in
    firmware; no schematic change).

### Power

11. Input. PJ-002A (C3096082). Pin 1 (centre, −) straight to GND. Pins 2 and
    3 both to 9V_JACK (one is the sleeve, the datasheet does not say which;
    tying both is harmless, as DrumSynthV3 did). 9V_JACK → SS34 Schottky
    (C8678) → 9V_IN; SMBJ15CA (C19077570) from 9V_IN to GND; 22 µF 25 V.
    fxbox's AO4606 bridge, SMBJ70CA and PE_SW/PE_BST nodes are dropped.
12. 9V_IN → AP63205 5 V buck (C2071056, L ANR5040T4R7M C7427135) → 5V_PRODUCT
    → TPS2116 (C3235557) with VBUS → 5V. 5V → AP63203 3V3 buck (C780769,
    L ANR5040T3R9M C7427132) and TLV75533 3V3A LDO (C404027). VDDA/VREF+
    through BLM18PG121SN1D ferrite, as fxbox.
13. Analog on 5V: OPA1678s, PCM1808 VCC through 10 Ω, relay coil. 3V3A: codec
    digital-analog, pots. USB firmware-only, DFU via BOOT0 button.

### Digital core (from fxbox)

14. STM32H743VIT6 LQFP-100 (C114409 tray / C5271084 reel), 25 MHz crystal
    X322525MOB4SI (C9006), VCAP 2×2.2 µF, BOOT0 pull-down + button for DFU.
15. USB-C C393939 16-pin, CC 5.1 kΩ (C23186), USBLC6-2SC6 ESD (C2687116).
16. SWD 1×4 header XFCN PZ254V-11-04P (C2691448; the fxbox C52016392 has no
    courtyard and failed import), NRST + SWO pads.
17. QSPI flash IS25LP064A (C2841348) for settings.
18. SAI1 pins as fxbox: BCLK PE5 (33 Ω series), LRCLK PE4, DAC SD PE6,
    ADC SD PE3, ADC SCKI PD14.

### Panel

19. 14 pots to one CD74HC4067 16:1 mux into one ADC pin. Pot part (owner
    2026-10-02): Alps RK09D1130C2P, LCSC C361173, 10 kΩ THT, Extended tier,
    taller shaft than recent builds. Same part for the analog Input Gain pot,
    15 in total. Catalog stock 174; a build of N units needs 15 N, so
    `jlc_stock_check` before ordering.
20. Three encoders on direct GPIO (owner 2026-10-02): MENU with push switch,
    KEY and SEMITONES without. Alps EC11N, shaft code 15: EC11N1525404
    (C470748, with switch) and EC11N1520401 (C470703, no switch), THT with
    side anchor legs, 30 detents. Datasheet heights: encoder tip 19.5 mm,
    bushing top 11.5 mm; pot tip 20.0 mm, bushing top 11.8 mm. Both shafts
    ø6 D-cut with a 4.5 mm flat, so one knob fits all. The 20-code EC11N
    parts are 5 mm too tall. Pot bushing figure read from a scanned drawing,
    ±0.2 mm.
21. Toggles (owner 2026-10-02): Yuen Fung ST-0-102-A01-T000-LF, LCSC
    C1788487, mini bat lever, SPDT ON-ON, vertical THT, 2.54 mm pitch, body
    8.6 mm, M5 bushing 8.6–14.2 mm above the board (spans the 11.8 mm panel
    plane; the printed panel hole captures it, no nut), lever tip 23.6 mm.
    Nine per board: eight inputs TOGGLE1..8 (TOGGLE1 = autotune) each with
    the common pin to GPIO, 10 kΩ pull-up to 3V3, 100 nF to GND, one throw to
    GND and the other no-connect; plus the bypass toggle in the relay block
    (item 7), common to the coil, one throw to 5V, the other no-connect. No
    LEDs on toggles; the lever shows the state. Datasheet gives AC ratings
    only; 5 V DC at 21 mA is far inside them.
22. OLED. 1.3" SH1106, I2C 400 kHz on an MCU I2C peripheral with free pins,
    4.7 kΩ pull-ups, 4-pin header C2691448 (3V3, GND, SCL, SDA). Module is
    hand-plugged; the header is assembled.
23. Input Gain pot is analog, in the preamp gain leg (item 4), not read by
    the MCU. JACK_TRS_N (item 1) and FX_ON_SENSE (item 7) are the two sense
    GPIOs.

### Mechanical

24. One board. Pots, encoder, toggles and OLED header stand up through the
    top; combo jack, 1/4" out, barrel and USB-C are right-angle out the rear
    wall. All panel jacks are THT so they cannot be pulled off the board.
25. Enclosure is 3D printed around the board. No footswitch.

## 3D model rule (owner 2026-10-02)

Every part that reaches the panel or a wall (pots, encoders, toggles, jacks,
USB-C, barrel) needs a 3D model whose height matches its datasheet before the
enclosure is designed. EasyEDA/JLC models are often the family's generic body:
the EC11N model is the 20-code shaft (24.5 mm tip, ours is 19.5) and the RK09D
model measured 34.8 mm. Procedure: fetch the manufacturer STEP for the exact
MPN; if none, build a simplified model from the drawing; in both cases measure
the STEP's z extent against the datasheet and record it in PARTS.md. The
toggle's EasyEDA STEP measured 23.6 mm and matches.

## Datasheet findings (2026-10-02)

- PJ-002A: centre pin Ø2.0 mm (datasheet), pin 1 = centre; pins 2/3 are a
  switch pair, sleeve not labelled. Resolved by design (item 11).
- NCJ6FA-H: holes 1, 2, 3, S, R, T, T, G plus 3 unlabelled (11 total, 6×Ø1.6,
  5×Ø1.2). Panel cutout Ø22 plus two Ø3.2 holes 19.8 mm apart; body 24.5 mm
  deep. NOT VERIFIED: which T is the normalling contact; which holes are
  anchors. Both resolved from the Neutrik drawing when the part card is written.
- NMJ6HCD2: T, R, S and TN, RN, SN rows 16.23 mm apart; Ø11.4 panel hole.
  N contacts are the normalling pair, left no-connect.
- G6K-2F-Y: coil 1 (+) / 8 (−), 237 Ω, 21 mA. Pole A COM 3, NC 2, NO 4.
  Pole B COM 6, NC 7, NO 5. NC/NO read from the drawn blade position.
- All jacks right-angle, mouth axis parallel to the board.
- NCJ6FA-H stock 111; re-check at order time, fallback NCJ6FA-V (C368485).
- Every jack, pot and encoder is Extended tier (~$3 each per unique part).

## Recommended next steps

1. Owner reports: Adam's list of toggled stages (which of TOGGLE2..8 are populated).
2. Library: drop NE5532DR and the probe headers, import OPA1678IDR and SS34,
   NCJ6FA-H part card + import, passives with Basic codes; PARTS.md.
3. JSON block spec from the Decisions above; `foreman schematic`; ERC 0;
   `foreman netlist check`; PDF render; read every sheet.
4. `foreman brief confirm` on the final hash.

## References

- Teensy-H7-Port (read-only): `hardware/fxbox/*.kicad_sch`, `PARTS.md`,
  `HANDOFF.md`; `hardware/h7core/`; `hardware/frontend/02-topology.md`,
  `04-parts.md`, `A4-audioin-parts.md`.
- ClaudeRouter: `canon.md` §XIV (schematic rules), `docs/board-recipe.md`,
  `docs/jlcpcb-parts.md`, `tests/fixtures/h7core-73036ce/`.
- CMYMini: `hardware/power.kicad_sch` (same AP63203 / USB-C / ESD parts).
- DrumSynthV3 (read-only): `CLAUDE.md` power section, FAB_AUDIT PJ-002A note.
