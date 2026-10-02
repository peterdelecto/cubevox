# cubevox hardware design

Owner interview 2026-10-02. This spec is the design brief for the KiCad
schematic. Parts are LCSC codes from the 2026-10-02 search; stock and pin
maps are re-verified live before the schematic is drawn (ClaudeRouter rule:
a part without a code is not a part).

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

## Decisions

### Signal chain

1. Combo jack. XLR pins 2/3 and TRS tip/ring feed the same balanced preamp
   input, pin 1 and sleeve to chassis/AGND. A TS plug shorts ring to sleeve,
   which grounds the cold input; that is the unbalanced case.
2. Preamp. Dual op-amp instrumentation amp, gain 0 to 60 dB on the Input
   Gain pot, input impedance about 20 kΩ so a pedal output is not loaded.
   Input protection as fxbox rev 3 (series R, BAT54S clamps C19726/C27675).
3. Bypass relay. Omron G6K-2F-Y DPDT 5 V (C47190). Pole A routes the preamp
   output either to the ADC input or to the output buffer. The coil is driven
   straight from the bypass toggle, no MCU in the path; one MCU GPIO senses
   the same line. Bypass therefore works with firmware crashed or booting.
4. ADC. PCM1808PWR (C55513), slave I2S, 24-bit, as fxbox. SCKI 12.288 MHz
   from PLL3 at 48 kHz (PLL3 M/N/FRACN recomputed from fxbox's 44.1 kHz values;
   firmware constant, no schematic change).
5. DAC. PCM5102APWR (C107671), no MCLK, SCK to GND, as fxbox.
6. Output buffer. Op-amp line/instrument buffer on the 5 V rail, ~100 Ω
   series out, DC blocked, to a 1/4" TS jack. Level is scaled in firmware
   (LINE / INSTRUMENT menu item); the DAC's ~2 Vrms full scale is the ceiling.
7. Output jack. Neutrik NMJ6HCD2 (C368502) or HOOYA PJ-611E (C309282), THT,
   right-angle. Pick on the datasheet anchor check.

### Power

8. Input. 9 V centre-negative barrel, CUI PJ-002A (C3096082). fxbox front
   end: AO4606 FET bridge accepts either polarity, SMBJ15CA TVS, AP63205 5 V
   buck, then AP63203 3V3 buck (C780769, L = ANR5040T3R9M C7427132) and
   TLV75533 3V3A LDO (C404027) for codec analog, VDDA/VREF+ and pots.
9. USB. Firmware only. TPS2116 (C3235557) selects USB VBUS or the 9 V-derived
   5 V, so USB alone powers the board for a DFU update.
10. Analog 5 V. One rail, mid-rail bias for preamp and output buffer, shared
    with PCM1808 VCC through its 10 Ω.

### Digital core (from fxbox)

11. STM32H743VIT6 LQFP-100 (C114409 tray / C5271084 reel), 25 MHz crystal
    X322525MOB4SI (C9006), VCAP 2×2.2 µF, BOOT0 pull-down + button for DFU.
12. USB-C C393939 16-pin, CC 5.1 kΩ (C23186), USBLC6-2SC6 ESD (C2687116).
13. SWD 1×4 header (C52016392), NRST + SWO pads.
14. QSPI flash IS25LP064A (C2841348) for settings.
15. SAI1 pins as fxbox: BCLK PE5 (33 Ω series), LRCLK PE4, DAC SD PE6,
    ADC SD PE3, ADC SCKI PD14.

### Panel

16. 15 pots to one CD74HC4067 16:1 mux into one ADC pin. Pot part (owner
    2026-10-02): Alps RK09D1130C2P, LCSC C361173, 10 kΩ THT, Extended tier,
    taller shaft than recent builds. Same part for the analog Input Gain pot,
    16 in total. Catalog stock 174; a build of N units needs 16 N, so
    `jlc_stock_check` before ordering.
17. KEY encoder with push, direct GPIO.
18. Toggles. One switch part for every toggled control; owner picks. Up to
    8 GPIOs reserved (autotune certain, bypass certain, others pending Adam).
    No LEDs on toggles; the lever shows the state.
19. OLED. 1.3" SH1106, I2C 400 kHz, 4-pin header (3V3, GND, SCL, SDA).
    Module is hand-plugged; the header is assembled.
20. Input Gain pot is analog, in the preamp gain leg, not read by the MCU.

### Mechanical

21. One board. Pots, encoder, toggles and OLED header stand up through the
    top; combo jack, 1/4" out, barrel and USB-C are right-angle out the rear
    wall. All panel jacks are THT so they cannot be pulled off the board.
22. Enclosure is 3D printed around the board. No footswitch.

## Verification debts (before the schematic)

- NOT VERIFIED: PJ-002A pin map (sleeve vs switch) and 2.0 vs 2.1 mm centre
  pin; read the datasheet, continuity-check the real part.
- NOT VERIFIED: NCJ6FA-H pinout (1, 2, 3, S, R, T, T, G); which T is the
  normalling contact.
- NOT VERIFIED: PJ-611E / NMJ6HCD2 pin map, switched tip, anchor style.
- NOT VERIFIED: every jack's mouth axis; the catalog says only "Right Angle".
- NCJ6FA-H stock is 111; re-check at order time, fallback NCJ6FA-V (C368485).
- Every jack is Extended tier (~$3 each per unique part on JLC Economic).

## Recommended next steps

1. Owner reports: toggle switch part, Adam's list of toggled stages.
2. Write the purpose line and `foreman brief` for the board.
3. Copy h7core/fxbox libs and spec into `hardware/`, run
   `assign_pin_types.py`, start PARTS.md.
4. Resolve the verification debts above from datasheets; `jlc_stock_check`
   every finalist.
5. Preamp and output buffer schematic block, then `foreman schematic`, ERC,
   render, review per block.

## References

- Teensy-H7-Port (read-only): `hardware/fxbox/*.kicad_sch`, `PARTS.md`,
  `HANDOFF.md`; `hardware/h7core/`; `hardware/frontend/02-topology.md`,
  `04-parts.md`, `A4-audioin-parts.md`.
- ClaudeRouter: `canon.md` §XIV (schematic rules), `docs/board-recipe.md`,
  `docs/jlcpcb-parts.md`, `tests/fixtures/h7core-73036ce/`.
- CMYMini: `hardware/power.kicad_sch` (same AP63203 / USB-C / ESD parts).
- DrumSynthV3 (read-only): `CLAUDE.md` power section, FAB_AUDIT PJ-002A note.
