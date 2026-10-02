# cubevox hardware

KiCad scaffold cloned from Teensy-H7-Port (read-only source, fxbox commit 48f1e82,
`hardware/fxbox` + `hardware/h7core`). No schematic or PCB exists yet.

## Contents
1. `cubevox.kicad_sym`, `cubevox.pretty`, `cubevox.3dshapes` - JLC parts from fxbox (20 symbols).
2. `cubevox_h7core.*` - same three for the h7core block (25 symbols, MCU, power, codecs).
3. `sym-lib-table`, `fp-lib-table` - both libs, `${KIPRJMOD}`-relative.
4. `cubevox.json`, `h7core_block.json` - fxbox and h7core `foreman schematic` specs, lib ids renamed.
   These still describe the fxbox panel and audio front end and must be edited.
5. `PARTS-fxbox-source.md` - fxbox parts notes, for reading.
6. `reference/` - fxbox schematics, unmodified, not the cubevox schematic.

## Recipe order (docs/specs/2026-10-02-hardware-design.md)
purpose line, `foreman brief`, PARTS.md with LCSC codes, `foreman schematic` from the
JSON spec, ERC 0 plus visual render, layout. Run `assign_pin_types.py` after any import.

## Tooling
kicad-cli 10.0.5 (/Applications/KiCad/KiCad.app).
Source had no tools/build_spec.py; fxbox.json is hand-kept (fxbox/tools has only routing scripts).
Not copied: .kicad_pcb, .kicad_pro, layout/, jlc_extra/ (holds one SMD tact switch).
