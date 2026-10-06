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
   crystal, USB-C DFU, SWD, ADC, DAC, 3V3 power and 9 V front end.
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
| 12 | 1/4" half of the combo | Mic or pedal, never both inputs at once. No pad; shared 20–50 dB preamp on the XLR leg, 1/4" through a 25.7 dB pad |
| 13 | Which H7 | Bare STM32H743VIT6 soldered on the product board, as fxbox. Not the WeAct module |
| 14 | Top face | Signal-flow row like the pedal view. Board 200 × 150 mm for now (owner 2026-10-02 pm). Orientation as the box sits: REAR edge at y = 0 (top of the KiCad view), knobs along the bottom toward the player, equally spaced for ergonomics; the owner lays out the knobs. Rear wall left→right as seen from the front: 1/4" out, USB-C middle, XLR combo, 9 V barrel far right. Every jack mouth faces the rear edge (−y) and sits proud of it (USB-C 1.5, barrel 1, combo flange 1, 1/4" nose 6 mm). Still provisional until Adam confirms |
| 15 | Gain structure | Two stages, 20.05 dB fixed + 0–29.9 dB on the pot. 1/4" contacts carry a 25.7 dB pad; menu item 1/4" INPUT: PEDAL / MIC adds 25.7 dB in firmware for a mic on a TRS cable; the combo's normalling contact tells the MCU a 1/4" plug is in |
| 16 | Power ground | Centre pin straight to GND, +9 V sleeve through a series Schottky and TVS. fxbox's either-polarity FET bridge is dropped because the Boss daisy chain and the audio cable share ground with the BD-2 |
| 17 | Toggle switch | Yuen Fung ST-0-102-A01-T000-LF (C1788487), the only ON-ON bat toggle on JLC whose bushing spans the 11.8 mm panel plane; lever tip 3.6 mm above the knobs |

## Decisions

### Signal chain

Revision 2 (2026-10-02 pm, after `hardware/SCHEMATIC-AUDIT.md`): op-amp,
bias, DC gain, reference, coupling, DAC filter and clock items below replace
revision 1. Disposition per finding is in `hardware/SCHEMATIC-AUDIT-RESPONSE.md`.

1. Combo jack NCJ6FA-H. XLR pin 2 → MIC_P, pin 3 → MIC_N, pin 1 and G to
   GND at the jack. TRS T → LINE_P, R → LINE_N through the 25.7 dB pad into the
   same preamp nodes, S to GND. BOTH T holes are the tip (Neutrik electrical
   diagram: one tip circuit, no switch; audit A01); both go to TRS_T. There is
   no plug detection; the 1/4" PEDAL / MIC menu item is a manual setting.
   JACK_TRS_N and R107 are deleted, PB11 is free. A TS plug shorts ring to
   sleeve, which grounds the cold input.
2. Input network, per leg (P and N identical). XLR leg: 47 Ω series, BAT54S
   clamps to 5VA/GND after it, 100 pF to GND, 100 kΩ to GND (defines 0 V on
   the jack side of the coupling cap), 47 µF 25 V electrolytic DC block (C120,
   C124; + on the IN_x side, which sits at VREF; corner 2.8 Hz against the
   1.2 kΩ bias leg) into node IN_x. TRS leg: 10 µF
   electrolytic (+ on the resistor side), 22 kΩ series into node IN_x. TRS_T
   and TRS_R each carry 100 kΩ to GND (R148, R149) so the coupling caps sit at
   a defined bias with no plug. Node
   IN_x: 1.2 kΩ to VREF. The 1.2 kΩ is both the mic bias path and the pad
   shunt: pad = 1.2/(22+1.2) = −25.7 dB (firmware MIC trim is +25.7 dB), mic
   input impedance 2.4 kΩ differential, pedal input impedance about 23 kΩ per leg.
   Electrolytics, not X7R, in every audio coupling position (audit A07).
3. Stage 1, fixed 20.05 dB. Instrumentation amp on OPA2197IDR (C139363,
   rail-to-rail input and output, 5.5 nV/√Hz, 25 µV offset; replaces OPA1678
   whose input range stops 2 V below V+, audit A02): A1 and A2 non-inverting
   with Rf 4.53 kΩ each and RG 1 kΩ + 22 µF 25 V bipolar electrolytic (C158,
   Panasonic EEEHP1E220P C413679, both polarities) in series
   between the inverting inputs, so DC gain is 1 and AC gain
   G = 1 + 2·4.53k/1k = 10.06 (20.05 dB), corner 7 Hz (audit A04). 47 pF across
   each Rf. Difference amp A3 with four 10 kΩ 0.1 % (audit A08), gain 1,
   reference VREF. Output PRE1, DC within a few mV of VREF.
4. Stage 2, 0–29.9 dB on the Input Gain pot (RK09D1130C2P 10 kΩ linear, wired
   as a variable resistor, wiper tied to one end, no end-stop). Non-inverting
   on A4 (OPA2197): the pot is the feedback resistor; R116 = 330 Ω from the
   inverting input to GND through 100 µF 10 V (DC gain 1, corner 4.8 Hz; the
   feedback current returns to ground, never into VREF, audit A03/A05).
   47 pF across the pot. G = 1 + Rpot/330 = 1 .. 31.3, so the stage runs 0 to
   29.9 dB and the chain 20.05 to 49.95 dB on XLR, −5.6 to +24.3 dB on the 1/4".
   At minimum gain the 330 Ω network loads the op-amp with 4.2 mA rms at
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
7. Bypass relay G6K-2F-Y DPDT 5 V (C47190). Pole A carries the audio: COM (3)
   = BUF_IN, NC (2) = PRE_OUT, NO (4) = DAC_ATT. Pole B is a dry contact: COM
   (6) to GND, NO (5) to FX_ON_SENSE, NC (7) no-connect, so FX_ON_SENSE reads
   low when the effect is on. FX_ON_SENSE has a 10 kΩ pull-up to 3V3 and
   100 nF to GND at the MCU. Coil + (1) from 5V through the bypass toggle,
   coil − (8) to GND, 1N4148W (C81598) flyback across the coil. Coil
   energised = effect on. No MCU in the switching path.
8. DAC. PCM5102APWR (C107671) as fxbox, output ground-centred ±3 V. OUTL →
   470 Ω → 2.2 nF to GND (TI figure 33 reconstruction filter, ~154 kHz, audit
   A09) → 10 µF 25 V electrolytic (+ on the divider side) → 10 kΩ / 15 kΩ
   divider to VREF (×0.6, 1.27 Vrms max) = DAC_ATT. Firmware LINE /
   INSTRUMENT menu scales digitally below that.
9. Output buffer. A5 (OPA2197) unity, input BUF_IN, output → 100 Ω → 10 µF
   25 V electrolytic (+ on the buffer side) → 100 kΩ to GND → OUT_TIP. Jack:
   HOOYA PJ-611E (C309282; owner 2026-10-02, replaces NMJ6HCD2) tip pin 6 to
   OUT_TIP, sleeve pin 2 to GND, pins 3/4/5/7 no-connect.
   Output mute: the node after the 10 µF and the 100 kΩ is OUT_AC. A second
   G6K-2F-Y DC5 (K102) pole A (COM 3 = OUT_AC, NO 4 = OUT_TIP) connects it
   to the jack. Pole B grounds the tip while muted: COM 6 = OUT_TIP, NC 7 =
   GND, NO 5 no-connect. Coil + (1) on 5V, coil − (8) on the collector of
   Q101 (MMBT3904, C20526) with a 1N4148W flyback (D109), a 1 kΩ base
   resistor from MUTE_N (PB5) and R153 100 kΩ from the base to GND. MUTE_N
   high closes pole A and releases the ground; the relay stays off while PB5
   floats at reset (PB4 and PA15 are avoided: their JTAG pull-ups are on at reset).
10. ADC/DAC clocks: SAI1 BCLK PE5 (33 Ω), LRCLK PE4, DAC SD PE6, ADC SD PE3.
    ADC SCKI comes from an SAI master clock coherent with BCLK/LRCLK, not a
    timer (audit I01): SAI1_MCLK_A on PE2 at 12.288 MHz for 48 kHz, pins per `hardware/PINMAP.md` "ADC master clock
    (I01)". PD14 is freed. Firmware: SAI1 master, PLL3 feeds the SAI1 kernel
    clock; mux settling ≥ 0.5 ms before reading a pot channel (audit I02);
    mute across the bypass transition with
    MUTE_N (audit I03; the relay switches on its own, the preamp path is alive
    only while analog power is).

### Power

11. Input. PJ-002A (C3096082). Pin 1 (centre, −) straight to GND. Pins 2 and
    3 both to 9V_JACK (one is the sleeve, the datasheet does not say which;
    tying both is harmless, as DrumSynthV3 did). 9V_JACK → SS34 Schottky
    (C8678) → 9V_IN; SMBJ15CA (C19077570) from 9V_IN to GND; 22 µF 25 V.
    fxbox's AO4606 bridge, SMBJ70CA and PE_SW/PE_BST nodes are dropped.
12. 9 V only (owner 2026-10-02). USB never powers the box: the TPS2116 mux
    is removed. 9V_IN → AP63205 5 V buck (C2071056, L ANR5040T4R7M C7427135)
    → 5V directly. USB-C is data only; VBUS goes to the USBLC6 ESD part and
    the VBUS_SENSE divider on PA9 (R6 33 kΩ / R7 82 kΩ, 3.6 V at 5 V VBUS), nothing else. PA9 is a GPIO input
    with native VBUS sensing disabled (AN4879). No VBUS-only mode. 5V → AP63203 3V3
    buck (C780769, L ANR5040T3R9M C7427132) and TLV75533 3V3A LDO (C404027).
    VDDA/VREF+ through BLM18PG121SN1D ferrite, as fxbox.
13. Analog on 5VA (5V through the ferrite): OPA2197s, PCM1808 VCC through
    10 Ω, relay coils on 5V. VA_SENSE: 5VA through 10 kΩ / 10 kΩ with 100 nF
    to PB1 (ADC12_INP5); R151/R152 sit at U106's 5VA pin, C163 at the MCU end.
    Pin map: `hardware/PINMAP.md` section 2. 3V3A: codec digital-analog, pots. USB
    firmware-only, DFU via BOOT0 button (the box must be on 9 V to run DFU).

### Digital core (from fxbox)

14. STM32H743VIT6 LQFP-100 (C114409 tray / C5271084 reel), 25 MHz crystal
    X322525MOB4SI (C9006), VCAP 2×2.2 µF, BOOT0 pull-down + button for DFU.
15. USB-C C393939 16-pin, CC 5.1 kΩ (C23186), USBLC6-2SC6 ESD (C2687116).
16. SWD on Tag-Connect TC2030-NL pads (J3), no header part. NRST + SWO pads.
17. No external flash (owner 2026-10-02). Settings, levels, the reverb engine selection and a few presets live in internal flash bank 2 (dual bank: write bank 2 while running from bank 1, no stall). Code and constants never live in bank 2.
18. SAI1 pins as fxbox: BCLK PE5 (33 Ω series), LRCLK PE4, DAC SD PE6,
    ADC SD PE3. ADC SCKI is SAI1_MCLK_A on PE2 (item 10, audit I01); PD14 is free.

### Firmware notes

F1. BOR level 3 (about 2.61 V falling).
F2. MUTE_N goes high 500 ms after the SAI clocks run, and low when VA_SENSE
    reads below 4.75 V (provisional, tolerance and shutdown time to budget).
F3. Assert XSMT (PE9) at least 3.4 ms before shutdown (PCM5102A, 48 kHz).
F4. PLL3 M/N/P 25/196/4 (DIVN3 = 195, DIVP3 = 3, FRACN3 4981, PLL3RGE = 0,
    PLL3VCOSEL = 1); SAI1 MCKDIV = 4, OSR = 0, NOMCK = 0.
F5. FX_ON_SENSE is active low.
F6. BYPASS outputs the preamp at pot level whatever the INSTRUMENT / LINE
    menu says, because the menu scales only the digital path.

### Panel

19. 14 pots to one CD74HC4067 16:1 mux into one ADC pin. Pot part (owner
    2026-10-03): Alps RK09D1130C2P, LCSC C361173, 10 kΩ linear, THT, Extended,
    $0.63 at 10+, stock 174. Alps drawing No. 4 (vertical, with collar) is
    shared with RK09D1130C1B; the only difference is shaft length LM1 25
    (C1B 20), D-flat ℓ1 12 (C1B 7). Shaft tip 25.0 mm, collar top 11.8 mm above
    the mounting surface, ø6 D-shaft with 4.5 mm flat. The owner's 13.5 mm on a
    sample was measured from the collar top (25.0 − 11.8 = 13.2). EasyEDA's
    models for C2P and C3C are swapped (the one titled C2P is the 20 mm shaft).
    Same part for the analog Input Gain pot, 15 in total; a build of N units
    needs 15 N. Over the 11.8 mm panel plane the shaft stands 13.2 mm.
20. Three encoders on direct GPIO (owner 2026-10-02): MENU with push switch,
    KEY and SEMITONES without. Matched to the pot (owner 2026-10-03): Alps
    EC11E vertical, operating length 20: EC11E15244B2 (C470754, push switch,
    1.5 mm travel) and EC11E15204A3 (C470710, no switch), 30 detents, 15
    pulses. Tip 24.5 mm, bushing top 11.5 mm, ø6 D-shaft 4.5 mm flat, flat
    length 12. PCB layout identical to EC11N (Alps drawings 4/5 vs 6/7: 5 × ø1
    at 2.5 pitch, legs 12.5 apart in 1.5 × 2.6), so the footprint is the Alps
    one; EasyEDA's EC11 footprint uses 2.54 pitch and is not used. Encoder tip
    sits 0.5 mm below the pot tip, bushing 0.3 mm below its collar.
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
    4.7 kΩ pull-ups. J117 is a hanxia HX PM2.54-1x4P TP-YQ 5 mm SMD socket
    (C42379197), JLC-assembled, that takes the module's own 4-pin header (GND,
    3V3, SCL, SDA). SMD because JLC's hand-inserted through-hole headers came
    out crooked on a previous board (owner 2026-10-06). The module is
    hand-plugged. With 9 mm standoffs the socket takes 6.5 mm of pin, so the
    module's pins must not reach more than 8.5 mm below its PCB.
23. Input Gain pot is analog, in the preamp gain leg (item 4), not read by
    the MCU. FX_ON_SENSE (item 7) is the only sense GPIO; JACK_TRS_N was
    deleted in revision 2 (item 1).

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
26. Mounting (owner 2026-10-02, option A): four M3 holes H101-H104 at 5 mm
    in from each edge (53.5/243.5 x 35/175), each with a 7 mm "m3seat"
    copper keep-out. No centre screw and no board keep-outs for the
    enclosure (owner 2026-10-02: the enclosure is designed around the
    board; any rest boss lands on whatever bare board is left). The panel toggles' nuts tie the panel
    to the board. Panel grid: eight stage columns at 24 mm pitch centred on
    the board (x 64.5 .. 232.5), rows y 115 (toggles), 142, 164/168.
27. Enclosure (owner 2026-10-02, option 1 = Hammond-1456-style console):
    three printed parts, modelled in Fusion from `hardware/exports/
    cubevox-board.step`. The board tilts 10 deg, rear up; the floor is
    horizontal and the front face vertical, so the box is 25 mm tall at the
    front and 53 mm at the rear, 213 x 157 in plan. The rear plate stays
    square to the board (leans back 10 deg) so every jack is square to it;
    it slides on over the proud jacks after the board is in and screws into
    corner blocks. The lid is 2 mm, underside 11.9 mm above the board (pot
    collar 11.85), with a 3.1 mm trough along the toggle row so the nuts
    get 3.4 mm of thread, an OLED window with a glass pocket, and four
    corner posts; one M3 x 20 per corner through lid, board and a heat-set
    insert in the tray boss. Board changes it asks for: J101 0.8 mm
    rearward (flange face 1 mm past the plate inner face), J108 2 mm
    rearward or keep the 1 mm nut pocket, OLED standoffs 9 mm. Pot shafts
    show 6.1 mm above the lid; a 1.5 mm lid or 25 mm pots would add more.

## 3D model rule (owner 2026-10-02)

Every part that reaches the panel or a wall (pots, encoders, toggles, jacks,
USB-C, barrel) needs a 3D model whose height matches its datasheet before the
enclosure is designed. EasyEDA/JLC models are often the family's generic body:
the EasyEDA EC11 model is the generic 24.5 mm body with a 2.54 mm-pitch footprint,
and EasyEDA's RK09D models for C2P and C3C are swapped. Procedure: fetch the manufacturer STEP for the exact
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

## Revision 3 parts (2026-10-02 pm)

47 µF 25 V ROQANG RVT1E470M0605 C72521 (6.3×5.4, Extended) ×2. 22 µF 25 V
bipolar Panasonic EEEHP1E220P C413679 (6.3×5.8, Extended) ×1. MMBT3904 C20526
(Basic). Resistors 0603: 4.53 kΩ C25971 (Extended), 22 kΩ C31850, 33 kΩ C4216,
82 kΩ C23254, 330 Ω C23138 (all Basic). 9.53 kΩ C23127 and 200 Ω C8218 are no
longer used. RVT1C220M0405 C72502 is no longer used, and RVT1E100M0405 C72484 drops to ×5.

## Datasheet findings (2026-10-02)

- PJ-002A: centre pin Ø2.0 mm (datasheet), pin 1 = centre; pins 2/3 are a
  switch pair, sleeve not labelled. Resolved by design (item 11).
- NCJ6FA-H: holes 1, 2, 3, S, R, T, T, G plus 3 unlabelled (11 total, 6×Ø1.6,
  5×Ø1.2). Panel cutout Ø22 plus two Ø3.2 holes 19.8 mm apart; body 24.5 mm
  deep. NOT VERIFIED: which T is the normalling contact; which holes are
  anchors. Both resolved from the Neutrik drawing when the part card is written.
- PJ-611E: pins 2..7 on a 6.4 x 11.2 mm grid, body 15.8 wide, front face 4.3 mm
  beyond the front pin row, thread 8.7 mm (replaces the NMJ6HCD2 note).
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
