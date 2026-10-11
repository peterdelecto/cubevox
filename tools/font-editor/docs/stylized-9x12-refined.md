# Stylized Refined 9X12 — style sheet

Rules for "Stylized Refined 9X12" (formerly "Stylized 9X12 Elegant"), the refined copy of "Stylized 9X12". Sources: Terminus 4.49.1 and Spleen 8x16
bitmaps read directly (research 2026-10-11). Owner picked direction A on 2026-10-11:
refined monoline, 2 px bars, spurs kept, 0 without a slash.

## Metrics
| Item | Rule |
|---|---|
| Cell | 9 x 12 px, editor spacing 1 px |
| Cap height | rows 0..11, every cap and digit |
| Baseline | row 11 |
| Body width | 8 px, columns 0..7; column 8 empty |
| Letter gap | 2 px (empty column 8 + 1 px spacing) |
| Counter | 3 px minimum, in both directions |
| Overshoot | none; rounds share cap height and baseline with H |
| Case | one case (owner); a..z are blank, text is drawn in capitals |

## Strokes
1. Vertical stems 2 px.
2. Horizontal bars 2 px.
3. Diagonals 2 px wide, stepping 1 px per row.
4. Crossings meet in a 3 px knot, never a 2 x 2 block plus spill.

## Curves
1. One 1 px bevel per rounded corner, then straight.
2. Closed rounds (O, Q, G, C) round the top-right and bottom-left corners and keep the
   top-left and bottom-right square. Spurs sit on the square corners.
3. Bowls (D, B, P, R) and 0 round with a 2-step bevel so they read softer than O.
4. Step runs only shrink toward the stem. A run that grows again after shrinking (1-2-1) reads jagged.

## Terminals
1. Strokes end flat.
2. Spurs stay (owner). A spur thickens a stem end 1 px inward for 2 rows, only at a
   top-left or bottom-right stem end.

## Disambiguation
| Pair | Rule |
|---|---|
| 0 / O | no slash; 0 rounds all four corners with a 2-step bevel, O has two square corners |
| O / Q | Q has a 2 px tail at bottom right |
| O / D | D has square left corners |
| 8 / B | 8 rounds every outer corner and pinches its waist; B has square left corners |
| 5 / S | 5 has a flat top bar and square upper left; S rounds both top corners |
| 2 / Z | 2 has a rounded top; Z has flat top and bottom bars |
| 1 / I | 1 has a flag; I has top and bottom bars |
| G / 6 / C | G has a bar at mid-right; 6 closes its lower bowl |
| U / V | U keeps stems to the bottom with one bevel |
| M / N | M's V stops at mid-height, at least 2 rows above the baseline; N's diagonal runs full height |

## OLED
1. Lit pixels bloom, so 1 px gaps close first. The 1 px waist of 8 and B is the weakest point.
2. Test on the real panel. If bars bloom, lower the SH1106 contrast register before redrawing.

## Proof
`node tools/font-editor/proof.js <library.json> --font "Stylized Refined 9X12" --compare "Stylized 9X12"`
must show no confusable pair closer than in the original.
