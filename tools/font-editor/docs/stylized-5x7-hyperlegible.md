# Stylized Hyperlegible 5x7 — style sheet

Menu and status font. A copy of the owner's "Stylized 5x7", which stays untouched.
Spec: `docs/specs/2026-10-11-font-sizes-design.md`. Library: `tools/font-editor/fonts/cubevox-fonts.json`.

## Metrics
| Item | Rule |
|---|---|
| Cell | 5 x 7 px, editor spacing 1 px |
| Line | 6 x 8 px pitch, 21 characters x 8 lines |
| Strokes | 1 px |
| Case | one case; a..z are blank, text is drawn in capitals |

## Changes from "Stylized 5x7"
| Glyph | Change | Separates |
|---|---|---|
| D | pointed right side (`###..` / `#..#.` / `#...#`) | D / O |
| B | square corners, top bowl narrower | B / 8 / E / R |
| 8 | X waist | 8 / 0 / B |
| 1 | longer flag, no foot | 1 / I |
| 5 | full top bar, hook at bottom left | 5 / S / 6 |
| 6 | diagonal top | 6 / 8 / 5 |
| S | flat bottom ending | S / 5 |
| A | open top left | A / 8 |
| G | round top left, long inner bar | G / O |
| U | round bottom on both sides | O / U |
| V | point starts at row 3 | U / V |
| J | no top bar | J / O |
| H M | spurs removed | H / N, M / N |
| N | diagonal runs full height | H / N |
| W | pointed feet | W / H / U |

## Proof
`node tools/font-editor/proof.js <lib> --font "Stylized Hyperlegible 5x7" --stem 1 --pairs`
must exit 0. Bloom is not gated at 1 px strokes, because one pixel of glow fills every 1 px counter.
