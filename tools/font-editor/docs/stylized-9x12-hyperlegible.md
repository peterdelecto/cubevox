# Stylized Hyperlegible 9X12 — style sheet

Legibility-first sibling of "Stylized Refined 9X12", drawn from it. Use is short panel
labels and numbers on the SH1106 1.3" OLED (0.23 mm pitch, white on black), read at a glance.
Where Refined trades a little distinction for family harmony, this font trades harmony back.

## Metrics
| Item | Rule |
|---|---|
| Cell | 9 x 12 px, editor spacing 2 px |
| Cap height | rows 0..11, every cap and digit |
| Body width | 8 px, columns 0..7; column 8 empty except Q tail, M, W, & |
| Letter gap | 3 px (empty column 8 + 2 px spacing) |
| Line | 11 px advance, 11 characters per 128 px line |
| Counter | 3 px minimum, both directions |
| Aperture | 2 px minimum (C G S 2 3 5 6 9) |
| Case | one case; a..z are blank, text is drawn in capitals |

## Strokes
1. Stems, bars and diagonals 2 px. No 1 px stroke except the ' " marks.
2. Crossings meet in a 3 px knot.

## Spurs
1. Kept only where they separate a pair. G keeps its bar, 1 keeps its flag, Q its tail.
2. H, N, M, W and D have no decorative spur (owner 2026-10-11).

## Pair splits
| Pair | Rule |
|---|---|
| 0 / O | 0 narrow (7 px) with all four corners rounded; O 8 px, two square corners; no slash |
| O / D | D straight spine, square left corners, 3-step bevel on the right |
| O / Q | Q bowl ends at row 9; a 2 px tail runs below it into column 8 |
| C / G | C keeps its top terminal; G has no top terminal, a beveled top right and a 2 px inward bar |
| 8 / B | 8 has an X waist; B square left corners and a top bowl narrower than the bottom |
| 3 / 8 / A | 3 flat top, open left side, middle bar starts at column 3 |
| 6 / 8 / 5 / G | 6 top is a diagonal stroke into the stem; 5 bowl hooks back up on the left |
| 9 / 8 | 9 open bottom-left |
| 5 / S | 5 square corners on the left; S rounds both ends |
| 2 / Z | 2 round top; Z flat top and bottom bars |
| 1 / I / ! / 7 / T | 1 long 2 px flag and no foot; I 6 px bars; ! 4 px wedge top, 3-row gap, dot; 7 diagonal runs to the baseline |
| E / F / P | F middle bar 5 px |
| A / 3 / 8 | A has no top-left terminal, so its top stays open |
| P / R / B | P bowl ends at row 6; R keeps the Refined bowl and diagonal leg |
| K / X | K straight stem, arms meet it at mid-height |
| M / N / H / W | M V stops at mid-height; N diagonal full height; H plain; W V rises from the baseline |
| U / V / Y / O | U bottom rounds in two steps; V pointed; Y arms meet at mid-height |

## Punctuation
1. Period, comma and colon dots are 2 x 2 with 2 px clear around them.
2. Minus is a 2 px bar, 6 px wide.

## Sources
1. Atkinson Hyperlegible, Braille Institute: https://brailleinstitute.org/freefont
2. Atkinson notes: https://pimpmytype.com/font/atkinson-hyperlegible/
3. Letter similarity review, Mueller & Weidemann 2012: https://obereed.net/lettersim/index.html
4. Light-on-dark weight: https://typedrawers.com/discussion/4138/white-on-black
5. Font grade by polarity: https://thereadabilityconsortium.org/wp-content/uploads/2023/07/How-bold-can-we-be-The-impact-of-adjusting-font-grade-on-readability-in-light-and-dark-polarities-1.pdf
6. Townsend 1971, Gilmore 1979 confusion pairs; Clearview halation; Pelli crowding (from recall).

## Proof
`node tools/font-editor/proof.js <lib> --font "Stylized Hyperlegible 9X12" --compare "Stylized Refined 9X12" --pairs`
must exit 0, which means every listed pair at blur 12 or more and bloom 10 or more.
Known near pairs outside the list: I/T 10.1 and 1/T 10.9, both no closer than in Refined.
Library: `tools/font-editor/fonts/cubevox-fonts.json`.
