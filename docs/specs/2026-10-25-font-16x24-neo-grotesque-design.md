# 16X24 neo-grotesque redraw

The readout's value font, `Myrtle 16X24`, drawn as a condensed
neo-grotesque from pixel recipes. The recipes live in `build/fontaudit/font16.py`
(gitignored); the glyph bitmaps in `tools/font-editor/fonts/cubevox-fonts.json` are what the
firmware builds from. Supersedes the generator notes in
`tools/font-editor/docs/stylized-hyperlegible-16x24-parameters.md`.

## History

1. 2026-10-25: drawn from the owner's counter brief, then settled glyph by glyph on named
   proposal sheets (`build/fontaudit/16x24c-proposals*.png`). Full proof:
   `build/fontaudit/16x24c-proof.png`.
2. A pasted rule set (2 px horizontals, corners 3, 1, 0, a 22 px cell, H 18 px wide) was drawn
   as a sample and judged a regression: hollow, top-light rounds and boxy corners. Not used.

## Rules

1. Counters are their own rounded rectangles, set in from the outer contour by the stroke.
   Square where a bowl meets a stem.
2. One outer corner on every round: a 2-step chamfer, 2 then 1 blank pixels. Counter corners
   take a 1 px cut; the m's 3 px counters and the & loop stay square. The 0 alone is rounder
   (rule 7).
3. Terminals: the 3 px wall runs three rows past the corner (rows 3-5 at the top, rows 18-20 at
   the bottom) and is cut flat, flush with its own bowl. A block set in from the wall left a
   1 px spur under the corner.
4. Diagonals hold one run width along the slope: 4 px at 1:1 and 2:1, 3 px at 3:1 and 4:1. N, X
   and the & leg run 2:1 at 3 px.
5. Monoline 3 px. Bars end in flat vertical cuts.
6. Cell 16 x 24; ink up to 15 px; column 15 blank. Text is spaced by ink with a 3 px gap and an
   8 px space; readout digits sit centred on an 18 px pitch.
7. The 0 has the O's width on rounder corners: outer 4, 2, 1, 1, counter 2, 1.

## Widths

| ink width | glyphs |
|---|---|
| 15 | 0 2 3 4 5 7 8 A B E F G H J M N O P Q R S T U V W X Y Z m % & / |
| 14 | 6 9 C D |
| 13 | # K L |
| 12 | b (flat) |
| 11 | I + - |
| 9 | 1 " |

## Per glyph

| glyph | construction |
|---|---|
| 0 | the O's width, rounder corners (rule 7) |
| 1 | stem on columns 7-9; flag a 4 px 1:1 diagonal from the stem's top to row 6, gap under it from row 5 |
| 2 | ring rows 0-9; diagonal 1:1 4 px from the counter's corner cut to the base |
| 3 | upper ring columns 1-14, lower ring 0-14, both open left; waist bar rows 9-11 from column 6 |
| 4 | diagonal into the stem's top on row 0, gap under it from row 4; bar rows 12-14 |
| 5 | top bar, vertical rows 3-7, shoulder rows 8-10, bowl open left |
| 6 9 | 14 px; the bowl's counter rounded at all four corners; 9 is the 6 turned over |
| 8 | upper ring columns 1-13 rows 0-11, lower ring rows 9-23; waist rows 9-11 |
| A V | 3 px legs, one column every four rows, 5 px flat apex; V is A turned over |
| B | upper bowl columns 0-11 rows 0-11, lower bowl full width rows 9-23; waist rows 9-11 |
| C | 14 px; terminals rows 3-5 and 18-20 |
| D | 14 px, square left |
| F | middle arm rows 8-10 to column 11 |
| G | terminal rows 3-5; bar rows 11-13 from column 5; the ring's wall under it as the spur |
| J | U's hook, top bar from column 6, square inside corner |
| K | symmetric about the middle: arm 2:1 from columns 9-12 to the stem on row 11, leg the arm turned over |
| M W | diagonals one column every three rows, closing in a 3 px point on rows 18-20 |
| N X | 2:1 diagonals at 3 px, corner to corner |
| P R | one 13-row bowl; R's leg 2:1 from the middle of the bowl's base to columns 11-14 on the baseline |
| Q | 13 px ring, full height; 1:1 tail from row 18 through the bottom-right stroke |
| S | the top half turned over is the bottom half; spine leaves the left wall on row 7 and enters the right wall on row 16, steep at both walls, flatter in the middle |
| b | the flat sign: stem; bowl joined square on rows 9-11; 1:1 diagonal into a point at the stem's foot |
| m | the minor-key m: arches joined flat across the middle stem, 3 px square counters |
| & | one stroke as in the owner's reference: the loop's left side runs on as the 2:1 leg to the bottom right; the right side crosses it under the loop, goes round the bowl and rises 1:1 into the arm |
| # | rows 2-21, 13 px wide |
| % | two 8 px rings at stroke 2, a 2:1 slash |

## Gate

1. `proof.js --pairs` passes every required pair. It drops 0/O for this font
   (`DROPPED_PAIRS` in `tools/font-editor/proof.js`): the 0 stays within 25 of the O against a
   bar of 42.7, and the letter O never appears in 16X24 text on the box, which draws digits,
   the units (in capitals: DB, MS, ST, %), the key names, BYPASS and MUTED.
2. `build/fontaudit/shape16.py` reports the largest solid block and each counter's straight
   share. Accepted: 5 x 5 blocks in M, W and Q; the 8's upper counter at 67-71 %; the 4's and
   the flat's counters, a triangle and a leaf by design.

## Readout

The widest key name, `F# / D#M`, measures 116 px; its field's left edge is x 6.

## Procedure

1. `python3 -B build/fontaudit/font16.py --apply` writes the glyphs into the library.
2. Export the header with `FontCore.toCHeader` under Node; `node tools/font-editor/check-header.js`.
3. `cmake --build build -j && ctest --test-dir build`; re-cut the goldens per item 13 of
   `2026-10-24-oled-gui-design.md`; `ctest` again; `OUT=/tmp/cubevox-fw-release firmware/build.sh`.
4. Proofs: `python3 -B build/fontaudit/proof16.py LIB OUT.png`; the gate as above;
   `python3 -B build/fontaudit/shape16.py`.
