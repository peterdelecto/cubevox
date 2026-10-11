# Stylized Hyperlegible 16X24 — style sheet

Value and large-label font. Spec: `docs/specs/2026-10-11-font-sizes-design.md`.
Library: `tools/font-editor/fonts/cubevox-fonts.json`. The owner refines it in the editor.
Drawn at its own size from the recipe below, with "Stylized Hyperlegible 9X12" (including the
owner's M, Q and W) as the design brief.

## Metrics
| Item | Rule |
|---|---|
| Cell | 16 x 24 px, editor spacing 2 px |
| Body | columns 0..14, rows 0..23; column 15 empty |
| Letter gap | 3 px |
| Line | 18 px advance, 7 characters per 128 px line |
| Digits | tabular; every digit uses the same cell |
| Case | one case; a..z are blank, text is drawn in capitals |
| Glyph set | `! " # % & ' ( ) + , - . / 0-9 : A-Z` (50) |

## Recipe
1. **Weight.** Stems and bars 4 px, 17 % of the cap height, the same share as 9X12's 2 of 12
   (owner picked 4 px over 3 px, 2026-10-11).
2. **Bars.** Top 0..3, middle 10..13, bottom 20..23. Glyphs whose 9X12 bar sits elsewhere move
   with it (G and 4 at 12..15, 5 at 8..11, B splits its middle bar 10..11 / 12..13).
3. **Centre stem.** Columns 5..8 (I, T, Y, 1, !, +, punctuation dots).
4. **Round corner.** Outside, a staircase flat, 1, 1 then vertical; inside, one chamfer pixel
   where a bar meets a stem. From Terminus u32b, which keeps this radius at every size.
   0 alone uses a 4-step staircase, so it reads rounder than O.
5. **Which corners round.** Each glyph rounds the same corners as its 9X12 source
   (O rounds top-right and bottom-left; C, G, S, E, A, U, J, 2, 3, 5, 6, 8, 9 follow theirs).
6. **Diagonals.** 45 degree strokes run 5 px per row and step 1 px per row (2, 6, Z, K, R leg,
   D's cut). Steep strokes run 4 px (7, N, V, X, Y, /, the 1 flag, brackets).
7. **D.** A 45 degree cut 7 rows deep at both right corners keeps 9X12's pointed D.
8. **M and W.** A solid wedge set off from the stems by a 1 px slit, apex at rows 10..11
   (M) and 12..13 (W), as in the owner's 9X12.

## Proof
`node tools/font-editor/proof.js tools/font-editor/fonts/cubevox-fonts.json --font "Stylized Hyperlegible 16X24" --compare "Stylized Hyperlegible 9X12" --stem 3 --pairs`
must exit 0. The stem lint only knows 1 to 3 px, so its STEM flags on this 4 px font are noise.
