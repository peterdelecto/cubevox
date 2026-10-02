# fxbox — parts pass

Vendor check, attribute readings and the three-record cross-check are in the register
(`layout/register.jsonl`, E0001–E0005, P0026). This file holds the footprint corrections
the parts gate requires.

## Footprint corrections (owner ruling 2026-09-12 for the gate; J102–J105 edits owner-approved 2026-09-24, register P0027)

`foreman parts fxbox.kicad_pcb` before: `model_courtyard_mismatch: 4` (J102–J105).
After: `model_courtyard_mismatch: 0`. Both edits are in the project library
`fxbox.pretty`; the board copies follow.

| Ref | What was wrong | Side corrected | Before | After | Source |
|---|---|---|---|---|---|
| J102, J103 (PJ-325M) | Model leads sat +1.00, −2.85 mm off their pads, so the body stood off the courtyard and the leads read as body 3.5 mm below the board. The courtyard also left out the threaded barrel at the mouth. | Model offset (moved) and courtyard (grown at the mouth) | offset (0, 0, 0); overhang left 2.96, top 2.75, below_board 3.5 mm; courtyard −x edge at −7.10 | offset (−1.000, −2.850, 0); model box within 0.1 mm of the courtyard; below_board none; courtyard notch x −11.10..−7.10, y −2.90..3.10 around the barrel | XKB drawing PJ-325MX A0: body 14.40 × 8.10, pin 1 at 1.20 from the mouth face, axis 3.20 from the body edge, M6 barrel 4.00 long. Model lead positions measured from the `.wrl`. |
| J104, J105 (PJ-611E) | Model leads sat −2.84, −2.85 mm off their pads; the leads read as body 3.2 mm below the board | Model offset (moved) | offset (0, 0, 0); overhang left 2.82, top 2.81, below_board 3.2 mm | offset (2.840, −2.850, 0); model box within 0.1 mm of the courtyard; below_board 0.44 mm (the nut rim, past the board edge, under the 1.0 mm threshold) | HOOYA PJ-611E rev A2: body 15.8 wide centred on the 11.2 mm pad rows, axis 4.2 mm from one row, front face 4.3 mm from the nearest pin column, thread 8.7 mm. Model lead positions measured from the `.wrl`. |

Renders, J102 and J104 alone at 0°, `layout/renders/`:

1. `parts-jacks-as-is-top.png`, `parts-jacks-as-is-front.png`, `parts-jacks-as-is-iso.png` show the pads exposed beside the bodies.
2. `parts-jacks-offset-fix-top.png`, `parts-jacks-offset-fix-front.png`, `parts-jacks-offset-fix-iso.png` show each body over its pads and each lead through its hole.

Mouth axes, from the renders and the drawings: PJ-325M at local −x, PJ-611E at local +x.
