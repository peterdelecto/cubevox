# Codex round 5: pre-fabrication audit

Board at git 135a888, 2026-10-06. The owner intends to order from JLCPCB (4-layer,
JLC assembly, single-side SMD on F.Cu) soon. Audit the board and schematic as they
stand and name anything that should stop the order or be fixed before it.

## Read these (all under `hardware/`, you are in that directory)

1. `cubevox.kicad_pcb` (2.1 MB s-expression, the board), `cubevox.kicad_sch` and
   `h7core_block.kicad_sch` (schematic), `cubevox.kicad_pro` (net classes, DRC rules).
2. `exports/cubevox-netlist.net`, `exports/cubevox-bom.csv`, `exports/cubevox-cpl.csv`,
   `exports/cubevox-drc.json` (0 violations, 34 unconnected items).
3. `PARTS.md`, `PINMAP.md`, `../docs/specs/2026-10-02-hardware-design.md`.
4. Prior rounds: `audit/2026-10-02-round2-response.md`, `audit/2026-10-02-round3-response.md`,
   `audit/2026-10-06-round4-routing-questions.md`, `audit/2026-10-06-stray-route-audit.md`,
   `SCHEMATIC-AUDIT-RESPONSE.md`. Do not repeat items those rounds already closed unless
   the board contradicts the closure.
5. `RUNLOG.md` tail (last 40 lines) for the latest decisions. Pending by design: the
   POT01..14 and TOGGLE1..8 nets are the 34 unconnected items; they wait on the panel
   column count. Everything else is meant to be final.

## Facts

STM32H743 LQFP-100 at 3V3 with VDDA/3V3A analog rail, AP63203 buck 9 V to 3V3,
TLV75533 for 3V3A with EN sequenced from a 3V3 supervisor (check it landed), PCM1808
ADC, PCM5102A DAC, OPA2197 analog chain at 5 V single supply, CD74HC4067 pot mux,
USB-C sense only with ESD array, OLED on an SMD socket, bypass relay, ST-NCJ6FAH
combo jack, Tag-Connect SWD. Stackup F.Cu / In1 GND / In2 GND / B.Cu, 3V3 and other
rails as traces not planes, 1.6 mm.

## Audit scope, in priority order

A. Schematic correctness: every IC pin tied as its datasheet requires (power, enable,
   mode, unused inputs, boot pins, oscillator loading), polarity of electrolytics and
   diodes, decoupling per pin, pull-ups on I2C and open-drain lines, power-up order,
   anything that would make the board not boot or damage a part on first power.
B. Netlist vs board: parts in the schematic with no footprint on the board or the
   reverse, footprints whose pad count or pin mapping does not match the symbol,
   nets shorted or split by copper that DRC would not catch (zone islands, wrong
   net on a pour).
C. Fabrication and assembly for JLC: footprint pad geometry vs the LCSC part in the
   BOM, missing or wrong LCSC numbers, parts without a rotation that JLC CPL can
   use, THT parts that JLC will hand-insert, silk on pads, fiducials, panel/edge
   clearance, mounting holes and keepouts, anything in `cubevox.kicad_dru`.
D. Routing risks not covered by DRC: analog returns crossing digital, the I2S bus,
   crystal, buck switch node, USB pair, ADC input RC, ground stitching at layer
   changes, long B.Cu hops listed in the stray-route audit (VIN_9V 155 mm on B.Cu,
   VA_SENSE, MUX_S0..S3, ADC_DOUT), trace widths vs current.
E. Anything else that would cost a re-spin.

## Reply format

Under 900 words. Sections A to E. Each item on one line:
`REF/net — FAIL | WARN | PASS — what, where (ref or coordinates), what to do`.
FAIL means stop the order. Finish with one line: ORDER | HOLD and the FAIL count.
No preamble, no restating the brief, no praise.
