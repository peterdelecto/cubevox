# Pre-fab audit brief, 2026-10-10 (read-only agents)

Board: `hardware/cubevox.kicad_pcb` at commit c2635eb. **4-layer**, JLC04161H-7628 stackup,
1.6 mm, HASL lead-free. F.Cu signal (1 oz), In1.Cu full GND zone (0.5 oz, 0.21 mm prepreg
below F.Cu), In2.Cu full GND zone (0.5 oz, 0.21 mm above B.Cu), B.Cu signal (1 oz); the outer
layers carry no GND fill, only a small /5V pour on F.Cu at the buck output. JLC fab + standard
PCBA, top side, THT included.
Schematics: `hardware/cubevox.kicad_sch` (root), `hardware/h7core_block.kicad_sch`.
Project: `hardware/cubevox.kicad_pro`. Parts list: `hardware/PARTS.md`. Pins: `hardware/PINMAP.md`.
Previous audits to read first so you do not repeat closed items:
`docs/audits/2026-10-09-prefab-mega-audit.md`, `docs/audits/2026-10-08-prefab-review.md`,
`docs/audits/2026-10-06-round5-prefab-response.md`, `docs/audits/2026-10-02-schematic-audit-response.md`.
The 2026-10-08 order (cancelled, re-order pending with v3 files) is described in `hardware/RUNLOG.md`
row "Boards ordered".

## Tools
KiCad python: `/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 -I script.py`
kicad-cli: `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli`
Filter its stderr noise: `2>&1 | grep -v -e Fontconfig -e wxApp -e assert`.
KiCad 10 API notes: `PCB_VIA.GetWidth(layer)` needs a layer; `pad.GetSize()` is unrotated;
zone fills via `zone.GetFilledPolysList(layer)`. Scratch scripts go in `/tmp/`, never in the repo.
Existing read-only checkers: `hardware/tools/gates.sh` (ALL PASS at c2635eb), `hardware/tools/mega_audit.py`
(16 routing sections, clean except decided items), `hardware/tools/make_bom.py` (production BOM/CPL).

## Rules
1. Read-only. Do not write to any file under `hardware/` except the one output named in your task.
   Never save the board. Never run anything with `--apply`.
2. Every finding carries evidence: reference, net, coordinates in mm, and the command or script
   that showed it. No finding without a number.
3. Classify each finding: FIX (a defect, state the fix), DECIDE (owner call, state the two sides
   in one line each and your recommendation), OK (checked, clean, one line).
4. Decide inside this brief and report once. No option lists, no questions back.
5. Assembly facts that are settled: JLC fits every SMT part and the THT panel parts (17 pots,
   9 toggles, the encoder, the OLED socket J117) on F.Cu. Owner-soldered and excluded from the
   JLC BOM and CPL: J101 combo jack, J106 barrel, J108 1/4" out, SWPWR1 slide switch, all on B.Cu.
   The SH1106 OLED module is owner-supplied. TP1-TP11 are in_bom no. Routing style (45s, pad
   entries, loops, jogs) is already audited, skip it.
6. Output is a Markdown file at the path named in your task, design-doc style: tables, numbered
   items, one line each. Finish with a `## Summary` of FIX count, DECIDE count, OK count.
7. Units on every number.

## JLC 4-layer capability (verify any figure you rely on at jlcpcb.com/capabilities)
| item | limit |
|---|---|
| min trace width, outer and inner | 0.09 mm |
| min clearance, outer and inner | 0.09 mm |
| min via drill | 0.20 mm |
| min via diameter | 0.40 mm (annular ≥ 0.10 mm) |
| min PTH drill | 0.30 mm, slot ≥ 0.65 mm |
| hole to hole, different nets | 0.50 mm |
| drill to inner-layer copper (other net) | 0.20 mm |
| copper to board edge, every layer | 0.30 mm |
| NPTH to copper | 0.254 mm |
| silk line width | ≥ 0.153 mm |
| silk text height | ≥ 1.0 mm |
| silk to pad | ≥ 0.15 mm, no silk on pads |
| solder mask dam | ≥ 0.10 mm |
| SMT part rotation | JLC CPL convention (make_bom.py applies offsets) |
