# Schematic audit response, revision 2

Audit: `SCHEMATIC-AUDIT.md` (2026-10-02). Design text: `docs/specs/2026-10-02-hardware-design.md`
(Decisions 1-13 and "Revision 2 parts"). Generator: `tools/build_spec.py`. Refs not listed keep their
revision 1 numbers; new refs are R144-R147 and C158-C161.

## Dispositions

| ID | Disposition | What changed |
|----|-------------|--------------|
| A01 | fixed in rev 2 | J101 pins 6 and 7 (both T pads) go to TRS_T. JACK_TRS_N, R107 and the PB11 port and child pin are deleted; U1 pin 47 is no-connect. Firmware note: the 1/4" PEDAL / MIC menu item is a manual setting. |
| A02 | fixed in rev 2 | U104, U105, U106 are OPA2197IDR (C139363). Footprint SOIC-8_L4.9-W3.9. |
| A03 | fixed in rev 2 | U106A follower drives R146 22 ohm into VREF with C160 10 uF electrolytic. The per-load 100 nF VREF caps C122, C126, C131, C133, C140 are deleted. |
| A04 | fixed in rev 2 | Stage 1 RG is R109 1 k in series with C158 22 uF (plus on A1_FB), DC gain 1. Input legs are AC-coupled: R144 and R145 100 k to GND on MIC_P and MIC_N define 0 V on the jack side. Stage 2 gain leg ends in C159 100 uF. |
| A05 | fixed in rev 2 | R115 100 ohm end-stop deleted. R116 is 200 ohm from G2_INV to G2_RG, then C159 100 uF (plus on the R116 side) to GND. The pot is the feedback resistor. Stage 2 runs 0 to 34.2 dB. |
| A06 | superseded | The TPS2116 mux, R8 and R9 are removed (owner 2026-10-02). The box runs from 9 V only, so the PR1 threshold and the USB-only mute rule no longer exist. |
| A07 | fixed in rev 2 | C120, C121, C124, C125, C139, C141 are 10 uF 25 V electrolytics (C72484). C137 and C138 stay X7R in the ADC feed, where both sides sit near 2.5 V. |
| A08 | fixed in rev 2 | R111-R114 are 10 k 0.1 % 25 ppm (C309083). |
| A09 | fixed in rev 2 | DAC_OUTL goes through R147 470 ohm and C161 2.2 nF to GND (TI figure 33) before C139. |
| I01 | fixed in rev 2, firmware open | ADC SCKI comes from SAI1_MCLK_A on PE2 (U1 pin 1). The QSPI flash is removed (owner 2026-10-02), so PE7-PE10 and PC11 are free (pins 37-40, 79). ENC_MENU_A moves to PB6 (pin 92), TOGGLE7 to PD11 (pin 58). PD12, PD13, PD14 are free. R27 33 ohm stays in series on the SCKI line. Correction to the audit wording: the inherited fxbox design clocked SCKI from PD14 as SAI3_MCLK_B (AF6, PLL3 P), not TIM4_CH3. The move removes the cross-block coherence question, since BCLK, LRCLK and MCLK now come from one SAI block and divider. PE2 AF6 is NOT VERIFIED against DS12110. |
| I02 | firmware note | Wait at least 0.5 ms after a mux address change before reading a pot channel. No hardware change. |
| I03 | firmware note, open | No hardware change. Relay transitions can click. Mute across the bypass transition where the firmware can. The NC path needs analog power, so it is not a power-off pass-through. |
| I04 | partly fixed | Fixed: stage 2 label and gain note on the PREAMP block, README stale line, PARTS.md assembly notes (J101, J106, J108, J117 owner-assembled, excluded from JLC BOM/CPL). Open: analog blocks are still label-connected rectangles, not redrawn on separate sheets. Open: no filtered JLC BOM export exists yet. Open: PARTS.md SWD header text is stale. |

## Board

`netlist sync` plus a footprint action list: 7 footprints removed (C122, C126, C131, C133, C140, R107, R115),
9 swapped in place (U104-U106, C120, C121, C124, C125, C139, C141), 8 added in the parked grid right of
the outline (R144-R147, C158-C161). No owner-placed footprint moved.

## NOT VERIFIED

1. VREF start-up and stability with the follower, 22 ohm and 10 uF (audit A03). No simulation, no bench.
2. THD and clipping at maximum gain, and PRE_OUT DC against gain setting.
3. CMRR with the 0.1 % difference amp resistors.
4. Relay pop on bypass, with and without firmware mute.
5. Clocks: SAI1_MCLK_A on PE2, PLL3 settings, MCKDIV. Needs DS12110 and a scope.
6. Electrolytic polarity on a real part: footprint pad 1 = plus from its silk, 3D model orientation not checked, no sample inspected.
7. Continuity of both J101 T pads to one node on a real NCJ6FA-H.
8. Board DRC and routing for the changed nets (no copper was touched). Courtyard fit of the parked electrolytics.
9. Stock: C23127 shows 90 and C23147 shows 303 on the import. Re-check at order time.
