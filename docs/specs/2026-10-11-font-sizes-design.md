# Font sizes for the 128 x 64 OLED — design

## Status
Renamed (owner 2026-10-29): the three shipped fonts are Myrtle 5x7, Myrtle 9X12 and Myrtle 16X24 in the library and `Myrtle5x7`, `Myrtle9X12`, `Myrtle16X24` in the header. The older Stylized Hyperlegible drafts keep their names.

## Goal
Three font sizes, each matched to one job on the SH1106 (128 x 64 px, 0.23 mm pitch,
white on black, read from about 70 cm). All three belong to the "Stylized" family, are
single case, and pass the proof gate before the owner refines them.

## Sizes
| Font | Cell | Pitch | Stroke | Screen fit | Cap height | Job |
|---|---|---|---|---|---|---|
| Stylized Hyperlegible 5x7 | 5 x 7 | 6 x 8 | 1 px | 8 lines x 21 chars | 1.6 mm | menus, lists, status line |
| Stylized Hyperlegible 9X12 | 9 x 12 | 11 x 14 | 2 px | 4 lines x 11 chars | 2.8 mm | effect names, knob labels |
| Stylized Hyperlegible 16X24 | 16 x 24 | 18 x 26 | 4 px | 2 lines x 7 chars | 5.5 mm | the value being turned |

The project uses the three Hyperlegible fonts (owner 2026-10-11, after refining them).
Stylized 9X12 and Stylized Refined 9X12 stay in the library but the firmware doesn't use
them. The owner removed Stylized 5x7 from the library. The editor's header export,
`tools/font-editor/fonts/cubevox_fonts.h`, holds every library font, one namespace each
under `cubevox::fonts`. The firmware uses the `StylizedHyperlegible*` namespaces.

## Weights
One weight per size. The selected menu row shows as inverse video, as the firmware does
today, so no bold is needed. A 3 px stem in the 8 px body of 9X12 would shrink counters to
2 px, and 2 px gaps bloom shut.

## Screen layouts the sizes must serve
1. **Knob screen.** 5x7 status line (8 px), 9X12 label (12 px), 16X24 value (24 px).
   That is 44 px of text plus 20 px for gaps and a value bar.
2. **Menu screen.** Eight 5x7 rows, the selected row inverted.
3. **Slot screen.** Four 9X12 rows naming the effect in each slot.

## Glyph sets
1. **5x7.** The 94 glyphs of the owner's "Stylized 5x7", single case (a..z blank).
2. **9X12.** Done (2026-10-11), 47 glyphs plus minus.
3. **16X24.** The full 9X12 Hyperlegible set plus `+ %` (owner 2026-10-11; an earlier
   value-only cut was Claude's, not the owner's). Longest value strings to fit in 7 chars:
   `-12 ST`, `250 MS`, `-6.5 DB`, `100 %`.

## Design rules per size
### 5x7 (revise the existing font)
1. Keep 1 px strokes; there is no room for 2 px in a 5 px cell.
2. Apply the 9X12 pair splits that fit: 0 narrower than O or dotted, Q tail outside
   the bowl, 1 with flag, 5 square top-left, 8 vs B by left corners.
3. Gaps between strokes at least 1 px; no 2 x 2 checkerboard.

### 16X24 (redrawn at its own size)
1. Drawn from a written recipe in `tools/font-editor/docs/stylized-16x24-hyperlegible.md`,
   with 9X12 Hyperlegible's shapes and pair splits as the brief. A grid stretch of 9X12 was
   tried and rejected (chopped corners, uneven diagonals, too light).
2. Body 15 px wide (cols 0..14), col 15 empty; spacing 2 gives a 3 px letter gap.
3. Stroke weight 4 px (17 % of height, matching 9X12; owner picked 4 over 3 from a sample).
4. Tabular digits: every digit the same width so a value doesn't jitter as the knob turns.

## Proof tool changes (`tools/font-editor/proof.js`)
1. Gate thresholds scale with cell area: bar = 12 blur, 10 bloom at 9 x 12, times
   (w x h) / 108. That gives 42.7 / 35.6 at 16 x 24. At stem 1 the blur smears every
   stroke over three columns, so the 5x7 gate uses a raw pixel difference of 16 % of the
   cell (6 px), the share the 9 x 12 blur bar implies (checked 2026-10-11).
2. A `--stem N` option sets the expected stroke width (1, 2 or 3) for the stem and thin lints.
3. A `--set digits` option limits the closest-pairs scan to digits and value glyphs.
4. A `screens.png` render composes the three layouts above at true scale and at 4x zoom,
   using all three fonts from one library.

## Steps
1. **Proof tool** changes above (Sonnet). Baseline runs on 5x7 and 9X12.
2. **16X24 style sheet** `tools/font-editor/docs/stylized-16x24-hyperlegible.md`, then draw
   all ~25 glyphs in the main loop. Gate after each family: digits, punctuation, units.
3. **5x7 revision.** Style sheet `stylized-5x7-hyperlegible.md`, single-case copy, pair
   splits, gate. The owner's original "Stylized 5x7" stays untouched; the revision is a copy.
4. **Screens proof.** Render `screens.png` and check each layout fits with the real labels
   and values from the effect registry.
5. **Deliver** one library, `tools/font-editor/fonts/cubevox-fonts.json`, holding the owner's
   originals, Refined 9X12 and all three Hyperlegible fonts. The owner loads it, then refines.
6. **Firmware (later, own spec).** The display runs U8x8 tile mode with an 8 x 8 font, which
   can't draw 9X12 or 16X24. Drawing them needs a 1 KB framebuffer renderer and
   `cubevox_fonts.h`. The owner signed off the Hyperlegible fonts on 2026-10-11.

## Who does what
1. Proof tool → Sonnet. Proof runs and renders → Haiku.
2. Style sheets and glyph drawing → main loop. Owner refines after all glyphs are drawn.

## Verification
1. `proof.js --pairs` exits 0 for each font at its scaled bar.
2. Lint: no stroke thinner than the size's stem width, apart from diagonals and punctuation.
3. `screens.png`: every layout fits, with no clipped text; I read it before reporting.
4. Owner reads the true-scale screens at arm's length; later on the real panel.
