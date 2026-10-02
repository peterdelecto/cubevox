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
| 14 | Top face | Signal-flow row like the pedal view. Board 200 × 150 mm for now (owner 2026-10-02 pm). Orientation as the box sits: REAR edge at y = 0 (top of the KiCad view), knobs along the bottom toward the player, equally spaced for ergonomics; the owner lays out the knobs. Rear wall left→right as seen from the front: 1/4" out, USB-C middle, XLR combo, 9 V barrel far right. Every jack mouth faces the rear edge (−y) and sits proud of it (USB-C 1.5, barrel 1, combo flange 1, 1/4" nose 6 mm). Still provisional until Adam confirms |
| 15 | Gain structure | Two stages, 20 dB fixed + 0–40 dB on the pot. 1/4" contacts carry a 20 dB pad; menu item 1/4" INPUT: PEDAL / MIC adds 20 dB in firmware for a mic on a TRS cable; the combo's normalling contact tells the MCU a 1/4" plug is in |
| 16 | Power ground | Centre pin straight to GND, +9 V sleeve through a series Schottky and TVS. fxbox's either-polarity FET bridge is dropped because the Boss daisy chain and the audio cable share ground with the BD-2 |
| 17 | Toggle switch | Yuen Fung ST-0-102-A01-T000-LF (C1788487), the only ON-ON bat toggle on JLC whose bushing spans the 11.8 mm panel plane; lever tip 3.6 mm above the knobs |

## Decisions

### Signal chain

Revision 2 (2026-10-02 pm, after `hardware/SCHEMATIC-AUDIT.md`): op-amp,
bias, DC gain, reference, coupling, DAC filter and clock items below replace
revision 1. Disposition per finding is in `hardware/SCHEMATIC-AUDIT-RESPONSE.md`.

1. Combo jack NCJ6FA-H. XLR pin 2 → MIC_P, pin 3 → MIC_N, pin 1 and G to
   GND at the jack. TRS T → LINE_P, R → LINE_N through the 20 dB pad into the
   same preamp nodes, S to GND. BOTH T holes are the tip (Neutrik electrical
   diagram: one tip circuit, no switch; audit A01); both go to TRS_T. There is
   no plug detection; the 1/4" PEDAL / MIC menu item is a manual setting.
   JACK_TRS_N and R107 are deleted, PB11 is free. A TS plug shorts ring to
   sleeve, which grounds the cold input.
2. Input network, per leg (P and N identical). XLR leg: 47 Ω series, BAT54S
   clamps to 5VA/GND after it, 100 pF to GND, 100 kΩ to GND (defines 0 V on
   the jack side of the coupling cap), 10 µF 25 V electrolytic DC block (+ on
   the IN_x side, which sits at VREF) into node IN_x. TRS leg: 10 µF
   electrolytic (+ on the resistor side), 10 kΩ series into node IN_x. Node
   IN_x: 1.2 kΩ to VREF. The 1.2 kΩ is both the mic bias path and the pad
   shunt: pad = 1.2/(10+1.2) = −19.4 dB (firmware MIC trim is +19.4 dB), mic
   input impedance 2.4 kΩ differential, pedal input impedance about 11 kΩ.
   Electrolytics, not X7R, in every audio coupling position (audit A07).
3. Stage 1, fixed 26 dB. Instrumentation amp on OPA2197IDR (C139363,
   rail-to-rail input and output, 5.5 nV/√Hz, 25 µV offset; replaces OPA1678
   whose input range stops 2 V below V+, audit A02): A1 and A2 non-inverting
   with Rf 9.53 kΩ each and RG 1 kΩ + 22 µF 16 V electrolytic in series
   between the inverting inputs, so DC gain is 1 and AC gain
   G = 1 + 2·9.53k/1k = 20.1 (26.0 dB), corner 7 Hz (audit A04). 47 pF across
   each Rf. Difference amp A3 with four 10 kΩ 0.1 % (audit A08), gain 1,
   reference VREF. Output PRE1, DC within a few mV of VREF.
4. Stage 2, 0–34 dB on the Input Gain pot (RK09D1130C1B 10 kΩ linear, wired
   as a variable resistor, wiper tied to one end, no end-stop). Non-inverting
   on A4 (OPA2197): the pot is the feedback resistor; R116 = 200 Ω from the
   inverting input to GND through 100 µF 10 V (DC gain 1, corner 8 Hz; the
   feedback current returns to ground, never into VREF, audit A03/A05).
   47 pF across the pot. G = 1 + Rpot/200 = 1 .. 51, so the stage runs 0 to
   34.2 dB and the chain 26.0 to 60.2 dB on XLR, 6.6 to 40.8 dB on the 1/4".
   At minimum gain the 200 Ω network loads the op-amp with 7 mA rms at
   1.4 Vrms, inside the OPA2197's 65 mA. Output PRE_OUT, DC at VREF.
5. VREF. 5VA through 10 kΩ / 10 kΩ, 10 µF at the divider, follower on one
   OPA2197 half, then 22 Ω into the VREF node with one 10 µF (the follower
   never sees the capacitance directly, audit A03). No per-load 100 nF caps.
   VREF carries bias only: the 1.2 kΩ input bias legs, the difference amp's
   reference resistor and the DAC attenuator's bottom resistor.
   Three OPA2197 duals in total (A1–A4, VREF follower, output buffer).
6. ADC feed. PRE_OUT → 100 Ω → 2.2 nF to GND → 10 µF X7R (C14860; both
   sides sit near 2.5 V so it carries no DC bias, and the AC across it is
   under 1 % of the signal against the ADC's 60 kΩ) → PCM1808 VINL, which is internally biased at VCC/2
   through 60 kΩ (datasheet). ADC is always fed, so firmware can meter in
   bypass. VINR: 10 µF to GND (datasheet figure 26 AC-couples both inputs).
7. Bypass relay G6K-2F-Y DPDT 5 V (C47190). Both poles wired in parallel for
   contact reliability: COM (3, 6) = BUF_IN, NC (2, 7) = PRE_OUT, NO (4, 5) =
   DAC_ATT. Coil + (1) from 5V through the bypass toggle, coil − (8) to GND,
   1N4148W (C81598) flyback across the coil. Coil energised = effect on.
   FX_ON_SENSE: coil + node through 10 kΩ / 18 kΩ to GND into an MCU GPIO
   (3.2 V when on). No MCU in the switching path.
8. DAC. PCM5102APWR (C107671) as fxbox, output ground-centred ±3 V. OUTL →
   470 Ω → 2.2 nF to GND (TI figure 33 reconstruction filter, ~154 kHz, audit
   A09) → 10 µF 25 V electrolytic (+ on the divider side) → 10 kΩ / 15 kΩ
   divider to VREF (×0.6, 1.27 Vrms max) = DAC_ATT. Firmware LINE /
   INSTRUMENT menu scales digitally below that.
9. Output buffer. A5 (OPA2197) unity, input BUF_IN, output → 100 Ω → 10 µF
   25 V electrolytic (+ on the buffer side) → 100 kΩ to GND → OUT_TIP. Jack:
   NMJ6HCD2 (C368502) T to OUT_TIP, S to GND, switch contacts no-connect.
10. ADC/DAC clocks: SAI1 BCLK PE5 (33 Ω), LRCLK PE4, DAC SD PE6, ADC SD PE3.
    ADC SCKI comes from an SAI master clock coherent with BCLK/LRCLK, not a
    timer (audit I01): SAI1_MCLK_A on PE2 at 12.288 MHz for 48 kHz, which
    moves QSPI to bank 2; pins per `hardware/PINMAP.md` "ADC master clock
    (I01)". PD14 is freed. Firmware: SAI1 master, PLL3 feeds the SAI1 kernel
    clock; mux settling ≥ 0.5 ms before reading a pot channel (audit I02);
    mute while VBUS-only (item 12) and across the bypass transition where it
    can (audit I03; the relay switches on its own, the preamp path is alive
    only while analog power is).

### Power

11. Input. PJ-002A (C3096082). Pin 1 (centre, −) straight to GND. Pins 2 and
    3 both to 9V_JACK (one is the sleeve, the datasheet does not say which;
    tying both is harmless, as DrumSynthV3 did). 9V_JACK → SS34 Schottky
    (C8678) → 9V_IN; SMBJ15CA (C19077570) from 9V_IN to GND; 22 µF 25 V.
    fxbox's AO4606 bridge, SMBJ70CA and PE_SW/PE_BST nodes are dropped.
12. 9V_IN → AP63205 5 V buck (C2071056, L ANR5040T4R7M C7427135) → 5V_PRODUCT
    → TPS2116 (C3235557) with VBUS → 5V. TPS2116 PR1 divider R8 = 36 kΩ /
    R9 = 10 kΩ so the 9 V-derived rail is preferred down to about 4.6 V
    (audit A06; the analog chain needs ≥ 4.61 V at 5VA). 5V → AP63203 3V3
    buck (C780769, L ANR5040T3R9M C7427132) and TLV75533 3V3A LDO (C404027).
    VDDA/VREF+ through BLM18PG121SN1D ferrite, as fxbox.
13. Analog on 5VA (5V through the ferrite): OPA2197s, PCM1808 VCC through
    10 Ω, relay coil on 5V. 3V3A: codec digital-analog, pots. USB
    firmware-only, DFU via BOOT0 button; firmware mutes audio whenever
    VBUS_SENSE shows USB-only power, because 5VA is not guaranteed then.

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

19. 14 pots to one CD74HC4067 16:1 mux into one ADC pin. Pot part: Alps
    RK09D1130C1B, LCSC C470304, 10 kΩ linear, THT, Extended, $0.84 at 10+,
    stock 986 live 2026-10-02. Alps's own STEP (file RK09D1130-F20) measures
    shaft tip 20.0 mm and collar top 11.85 mm above the mounting surface,
    ø6 D-shaft with 4.5 mm flat, matching the encoder (19.5 / 11.5). Same
    part for the analog Input Gain pot, 15 in total; a build of N units needs
    15 N. Supersedes RK09D1130C2P (C361173, owner pick earlier the same day):
    Alps's F25 model shows a 25 mm shaft and the owner measured 13.5 mm on a
    sample, so that code is not trusted. Measure one C470304 sample before
    ordering fifteen.
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

24. One board. Pots, encoders, toggles and OLED header stand up through the
    top panel, which sits about 11.8 mm above the board. The combo jack
    (25 mm flange, 29 mm tall with its latch), the 1/4" out and the barrel
    jack hang from the UNDERSIDE (B.Cu) and face the rear wall, because on
    top they would come up through the panel (owner 2026-10-02). These three
    are through-hole and the owner hand-solders them; JLC's job stays
    single-sided Economic. USB-C (1.6 mm, SMD) stays on top and is
    JLC-assembled, so the rear wall has holes at two heights. All panel jacks
    are THT so they cannot be pulled off the board.
25. Enclosure is 3D printed around the board: about 12 mm above the board
    for the controls, 1.6 mm board, about 29 mm below for the combo jack,
    plus walls. No footswitch.

## 3D model rule (owner 2026-10-02)

Every part that reaches the panel or a wall (pots, encoders, toggles, jacks,
USB-C, barrel) needs a 3D model whose height matches its datasheet before the
enclosure is designed. EasyEDA/JLC models are often the family's generic body:
the EC11N model is the 20-code shaft (24.5 mm tip, ours is 19.5) and the RK09D
model measured 34.8 mm. Procedure: fetch the manufacturer STEP for the exact
MPN; if none, build a simplified model from the drawing; in both cases measure
the STEP's z extent against the datasheet and record it in PARTS.md. The
toggle's EasyEDA STEP measured 23.6 mm and matches.

## Revision 2 parts (2026-10-02 pm)

OPA2197IDR C139363 ×3 (Extended, $1.26, 19.8 k stock live). 10 kΩ 0.1 % 25 ppm
Viking ARG03BTC1002 C309083 ×4. Electrolytics ROQANG 105 °C: 10 µF 25 V
RVT1E100M0405 C72484 (4×5.4) ×7, 100 µF 10 V VT1A101M0505 C191859 (5×5.4)
×1, 22 µF 16 V RVT1C220M0405 C72502 (4×5.4) ×1. Resistors 0603: 470 Ω C23179,
200 Ω C8218, 22 Ω C23345 (Basic), 36 kΩ C23147 (Preferred), 9.53 kΩ C23127
(Extended). OPA1678 C192421 and the X7R coupling caps in the audio path are
superseded. TLV9062 C398356 stays the fallback op-amp.

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
- NCJ6FA-H stock 385 (2026-10-02 pm); re-check at order time. NCJ6FA-V
  (C368485) is NOT a fallback: it is the vertical version, wrong mounting
  angle for a rear-wall jack (owner 2026-10-02).
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
