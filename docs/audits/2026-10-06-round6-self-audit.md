# Round 6: own audit, board after D0359 (6e340e4)

Two tracks: an Opus agent read the netlist pin by pin against the datasheets; the lead ran
`foreman audit`, `foreman gates`, kicad-cli DRC/ERC and targeted pcbnew checks.

Verdict: no FAIL. Open items below need an owner call or a bench check.

## Schematic (netlist evidence)
1. BUF_IN (K101.3, U106.5) — WARN — no DC path while K101 is in transit, and the two throws sit on
   different bias nets (R123.2 on BIAS_W, PRE_OUT on VREF), so each bypass switch can click.
   Fix: R123.2 to VREF, or ~100k from BUF_IN to VREF.
2. D107/D108 on MIC_P/MIC_N — WARN — clamps sit on the 0 V side of the 47 uF coupling caps; they
   clip negative swings past ~-0.2 V. Fine for an SM58, distorts line level into the XLR.
3. Pot top (3V3A) vs ADC VREF+ (VDDA from 3V3) — WARN — not ratiometric; end-stops land a few
   counts off. Firmware calibrates the ends, or VREF+ moves to 3V3A.
4. J117 pin order GND/VCC/SCL/SDA — WARN — matches PARTS.md; many 1.3" SH1106 modules are
   VCC/GND. Check the purchased module before first power.
5. Not verified from a datasheet: G6K-2F-Y, PJ-002A, NCJ6FA-H, PJ-611E pin numbers, USB-C map.

## Board
1. Y1 — WARN — I2S_BCLK crosses under the crystal on B.Cu near (91.85,100.55). Two solid GND planes
   sit between; low risk, but the clean fix is a re-route around the crystal box.
2. HSE_IN 10.45 mm vs 8 mm budget; airline floor 9.57 mm — WARN — placement-limited; normal for a
   25 MHz HSE with 15 pF loads.
3. I2S_LRCLK 64 mm vs 60 mm budget — WARN — 33 R series at the source; acceptable.
4. Decoupling — PASS — VDDA has 100 nF at 2.4 and 3.7 mm (bulk farther), ADC_VCC 100 nF at 2.45 mm,
   C25 at 2.2 mm from U4. The foreman's gate 63 measures the bulk caps.
5. VBUS 0.2 mm vs 0.3 mm for 0.43 A (gate 47) — PASS — VBUS is sense only, microamps.
6. Connectors not on the outline (gate 37) — PASS — J1, J101, J106, J108 overhang the top edge by
   1.5 to 8.4 mm, as designed.
7. Body gaps -0.05 mm C56/C58/C61 to U1 — PASS — box measure only; pads clear, DRC courtyard
   overlap (error severity) reads 0.
8. 23 right-angle corners (canon 44) and 6 off-centre pad entries on the buck — WARN — cosmetic
   at these speeds; the owner's 45-only rule is not met at those spots.
9. GND edge stitching (gate 21) — WARN — 2 ties around a 695 mm perimeter. Canon 21 asks for
   5-10 mm along edges. Low cost to add before order.
10. D104-D109 silk polarity not visible (gate 89) — WARN — JLC places from CPL, but visual
    inspection and hand rework need the mark.
11. 9 lib_footprint_mismatch warnings (C121, C125, C139, C141, C160, C161, R147, U106, J117) — open
    from round 5 fixes; per-footprint diff still owed.
12. DRC 0 errors, 34 unconnected (pending pot/toggle nets); ERC 0; via audit 298 vias, 0 orphans.
