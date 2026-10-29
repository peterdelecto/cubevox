# Myrtle 16X24, parameter sheet

The readout's value font. Drawn from pixel recipes in `build/fontaudit/font16.py` (gitignored)
and written into the editor library; the glyph bitmaps in `fonts/cubevox-fonts.json` are the
source the firmware builds from. Spec: `docs/specs/2026-10-25-font-16x24-neo-grotesque-design.md`.

## Model

| parameter | setting |
|---|---|
| model | neo-grotesque, condensed to a 16 x 24 cell with column 15 blank |
| weight | monoline 3 px; horizontals, verticals and diagonals all 3 px |
| width | most capitals and figures 15 px; 6 9 C D 14 px; # K L 13 px; I + - 11 px; 1 " 9 px |
| outer corners | one 2, 1 chamfer on every round; the 0 alone takes 4, 2, 1, 1 |
| counters | their own rounded rectangles with a 1 px cut, square where a bowl meets a stem; the m's 3 px counters and the & loop stay square; the 0's counter takes 2, 1 |
| terminals | 3 px wall, three rows past the corner (rows 3-5 at the top, 18-20 at the bottom), cut flat and flush with its own bowl |
| diagonals | 4 px runs at 1:1 and 2:1; N, X and the & leg 3 px at 2:1 |
| bars | H bar rows 9-11; E arm rows 9-11 to column 13; F arm rows 8-10 to column 11; 8 and B waist rows 9-11 |
| spacing | by ink, 3 px gap and an 8 px space; readout digits centred on an 18 px pitch |

## Departures from the model

1. I carries serifs, so I, 1 and l stay apart.
2. The 0 is the O's width on rounder corners. 0/O is dropped from the gate for this font,
   because the letter O never appears in 16X24 text on the box.
3. The b cell holds the flat sign and the m cell the minor-key m.

## Legibility pairs

The gate (`node tools/font-editor/proof.js ... --stem 3 --pairs`) passes every required pair
except 0/O, which `DROPPED_PAIRS` in `proof.js` drops for this font. The pairs that set the
shapes are O/D (D 14 px, square left side), O/C (C 14 px, flat terminals), P/R (one 13-row
bowl, R's 2:1 leg), E/F (E arm 14 px on rows 9-11, F arm 12 px on rows 8-10), 8/B (B's upper
bowl two columns narrower) and G/6 (G's bar on rows 11-13 from column 5).
