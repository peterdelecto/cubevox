# OLED GUI, runtime screens

Owner brief 2026-10-24, with the foreman's decided answers of 16:05. The box shows three runtime
screens (voice, readout, MUTED) and the menu. Every change between screens is one 2-frame dissolve.
The pixel reference is `tools/oled-proof/oled_motion.cpp`, which draws on the firmware's own canvas
and fonts. The menu is `2026-10-24-polish-design.md` and does not change.

## Status
1. Host part built and tested 2026-10-24. The readout and MUTED frames equal the proof renderer byte for byte.
2. The owner approved proofs 3 to 10 on 2026-10-24 with one change, "we don't want re-centering". The value row now sits in a fixed field per parameter, as set out under Value field. The proof renderer and the goldens follow it.
3. Bench part open. The list is in Bench, below.

## Screens
| Screen | Shows | Rows |
|---|---|---|
| Voice | the input level as a ribbon scrolling right to left | horizon at row 32, up to 22 px each side, 1 px rim |
| Readout | the knob that moved last | card line 5x7 at (1, 1), name 9X12 top at 15, value 16X24 top at 34 |
| Muted | the word of the top switch, nothing else | 16X24 centred both ways, top at 20 |
| Menu | the list, chain view or update confirm | as the menu spec, unchanged |

## Priority
1. The open menu wins over everything, then a live readout, then the top switch, then the voice.
2. A readout is live from a knob move until 2000 ms after the last move.
3. A knob turned while the switch is on raises the readout. The chain keeps running and the knobs still apply when muted.
4. The readout falls back to the resting screen, which is MUTED while the switch is on and the voice otherwise.
5. The word comes from `Settings::topSwitch`. `Mute` reads MUTED and `Bypass` reads BYPASS.
6. The switch is on when `muteActive()` is true under `Mute`, and when `muteBypassOn()` is true under `Bypass`. `topSwitchEngaged` holds that rule.
7. A knob moved under the open menu updates the readout state but does not show it. Closing the menu inside the hold shows the readout again.

## Readout
1. Row one is the slot number, a space and the card name, 5x7 at (1, 1).
2. Row two is the parameter name, 9X12, centred, top at row 15.
3. Row three is the value, a space and the unit, 16X24, with its top at row 34, in a 24 px band that spans rows 34 to 57. It sits in a fixed field, set out under Value field.
4. A text value (the autotune key) has no unit and does not roll. It is left-aligned in a field as wide as the widest name.
5. The value eases to the knob with a 20 ms time constant, which closes 86.5 % of the gap per tick.
6. Each tick the value sits between two whole numbers. A cell that differs between them rolls up out of its place while the next glyph rolls in from below. Both are clipped to the band.
7. The roll offset is `round(fraction * 24)` px. A cell that is equal in both numbers draws plain.
8. A new readout starts at the knob's reading. A different knob, or a readout that had gone, never eases from the old value.

## Value field
The value row never moves sideways. Each parameter has one field, computed once from its widest possible rendering and centred once. The current value only fills the cells of that field.

1. A numeric field is a sign cell, the digit cells, a space and the unit.
2. The sign cell exists when the range reaches below zero. It holds `-`, `+` or a blank for zero, and it stays in the leftmost cell.
3. The digit cells are as many as the digits of the widest number of the range. A parameter with decimals adds a cell for the point and one for each decimal.
4. A cell is 18 px wide, a 16 px glyph cell and 2 px of spacing, and a glyph's ink is centred in its cell.
5. The field is `cells * 18` px plus the width of the space and the unit. Its left edge is `(128 - width) / 2`, taken once. The space and the unit start at the left edge plus `cells * 18`.
6. A number is right-aligned in the digit cells, and a shorter number leaves its leading cells blank. `+5 ST` is the sign cell `+`, a blank tens cell and `5` in the ones cell. `0 ST` is two blank cells and `0`.
7. A number that gains a digit lights its leading cell and nothing else shifts. On a percent knob 99 to 100 rolls a `1` into the blank leading cell while each 9 rolls to 0 in its own cell.
8. A name field (the key) is as wide as the widest name, which is `F# / D#M` at 121 px. A name starts at the field's left edge, and a shorter name leaves the right of the field blank.

| Knob | Range | Cells | Field width | Left edge |
|---|---|---|---|---|
| Mix, Depth, Rate, Drive, Tone, Time on reverbs | 0..100 % | 3 | 77 px | 25 |
| Threshold | -70..-10 dB | sign and 2 | 95 px | 16 |
| Semitones | -12..+12 ST | sign and 2 | 95 px | 16 |
| Slapback Time, Response | 60..250 and 1..500 ms | 3 | 95 px | 16 |
| Decay | 5..1000 ms | 4 | 113 px | 7 |
| Key | 12 names | none | 121 px | 3 |

## Value formatting
All of it comes from the card's parameter table in `core/registry`, never from the screens.

| Parameter | Rule | Example |
|---|---|---|
| Continuous | rounded whole number of the unit | `210 MS`, `65 %`, `-24 DB` |
| Stepped number | the step's whole number | `+5 ST`, `0 ST`, `-12 ST` |
| Text | the name at the step | `F / DM` |
| Decimals | `KnobDesc::decimals` digits after the point, shown as tenths and so on | none declared yet |

1. A positive number carries `+` only when the parameter's range reaches below zero.
2. Zero carries no sign.
3. The font draws lowercase as capitals, so the table keeps its natural case.
4. `knobCells` gives the cells of a parameter and `formatCells` fills them. `formatShown` prints the same number as one string.
5. `core_registry.cpp` checks that every knob's field fits the panel in 16X24 with 1 px each side. The widest are `1000 MS` at 113 px and `F# / D#M` at 121 px.

## Voice
1. The audio interrupt sums the squares of the input samples of every block. The main loop takes the sum and the sample count at each UI tick.
2. The RMS of that window maps to 0..1. It is linear in dB from -50 dBFS (0) to -6 dBFS (1), raised to the power 0.6.
3. The level rises at once and falls with a 120 ms time constant, counted in real time so a late tick falls further. A level under 0.01 snaps to 0.
4. A tick turns the level into a half-height of `round(level^0.8 * 22)` px. Each height spans 2 columns, the newest at the right edge, so the ribbon scrolls 2 px a tick, which is 50 px/s.
5. A column is solid from `32 - h` to `32 + h`. A 1 px rim above and below it is lit where the Bayer threshold of the pixel is under 32, which is half of them.
6. At height 0 the column is the one pixel on row 32. In silence the screen is the line and nothing else.
7. The ribbon keeps filling while another screen shows, so the voice returns with its history.
8. The input level is measured where the samples enter the chain, so the bench clip moves it too.

## Dissolve
1. A screen change shows two mixed frames and then the new screen. At 25 fps each frame lasts 40 ms, so the mix takes 80 ms.
2. Mixed frame 1 takes the new screen's pixels whose Bayer 8x8 threshold is under 21 of 64. Frame 2 takes those under 43.
3. A canvas byte is one column of one 8-row page, so the mask for a byte is one precomputed byte, indexed by `x & 7`. The mix is `(new & mask) | (old & ~mask)` per byte.
4. Mask 1 lies inside mask 2. Frame 2 therefore works from frame 1's result in place, since every pixel outside mask 2 is still the old screen. One extra 1 kB canvas holds the new screen.
5. A change that arrives during a dissolve starts a new one from the frame on the panel.
6. Dissolves cover readout in, one slot's readout replaced by another's (another knob of the same slot included), readout out, mute on and off, and the menu opening and closing.
7. The first frame after the boot splash or the bench pattern dissolves from them.
8. The menu's own redraws (cursor, value, sub-screens) do not dissolve. They show at the next tick.

## Burn-in
1. The voice screen moves 1 px every 60 s. It starts centred, then circles the 8 outer cells of a 3 by 3 block, one pixel along one axis per step.
2. The shift moves the finished picture, so the rim dither moves with the ribbon. There is no animation.
3. The count starts at the first tick and survives the millisecond counter wrapping.

## Timing
| Item | Value |
|---|---|
| UI tick | 25 fps, 40 ms |
| Tick gate | 40 ms since the last tick and no dirty page left on the canvas |
| Dissolve | 2 mixed frames |
| Readout hold | 2000 ms after the last move |
| Value easing | 20 ms time constant |
| Level release | 120 ms time constant |
| Walk step | 60 s |
| Panel flush | one dirty page per loop pass, about 25 ms for a full frame at 400 kHz |

1. The gate keeps a tick from outrunning the flush, which would tear the picture. A write that fails leaves its page dirty, so ticks wait until it goes through.
2. Without a panel (no SH1106 answers) the face does not run. The stub display would log every page of the moving ribbon to the serial port.

## Modules
| Module | Owns |
|---|---|
| `core/level` | sum of squares, RMS, dB to level, the meter |
| `core/canvas` | `setPixel`, clipped digits, `copyFrom`, `dissolve`, the Bayer table and the dissolve masks |
| `core/registry` | `KnobDesc::decimals` and `labels`, `knobDesc`, `knobShown`, `formatShown`, `knobLabel` |
| `core/knob_filter` | `KnobBank::takeTouch`, one touch per move |
| `ui/gui` | the state machine: which screen, which dissolve step, the eased value, the walk |
| `ui/screens` | `renderVoice`, `renderReadout`, `renderMuted`, the ribbon history |
| `ui/face` | the top-level draw: the tick gate, the level meter, the screen pick, the dissolve onto the panel |
| `ui/menu` | `Menu::draw` draws the open menu only, `Menu::open` |
| `board/controls` | `controlsTakeTouch` |
| `board/audio` | `audioTakeInputEnergy` |

## Data flow
```
audio IRQ, per block ── sum of squares ──▶ audioTakeInputEnergy ─┐
board/controls ── touch (slot, knob) ──▶ Face::knobMoved ─────────┤
Settings, mute state, Menu::open ────────────────────────────────┴▶ Face::tick
   ▶ Gui::tick ▶ screen, dissolve step ▶ render into target canvas ▶ dissolve onto the live canvas ▶ flushStep
```

## Tests and proofs
1. `test/firmware/golden/*.bin` hold 27 frames of `oled_motion` (1024 page-layout bytes each). `tools/oled_golden.py` cuts them from a run of the proof renderer.
2. The host tests match them byte for byte: the four settled readouts, nine rolling frames of 120 to 210 MS, eight rolling frames of 0 to -12 ST, MUTED and its two dissolve frames, and one readout replaced by another.
3. The fixed field changed 11 of the 27 frames, and the other 16 stayed byte-identical. The proof renderer's `rollValue` follows the same rule, and the goldens were re-cut from it.

| Frames | Why they changed |
|---|---|
| `settled-65pct` | the field has 3 digit cells, so 65 has a blank leading cell and sits half a cell to the right |
| `roll-semi-6`, `-12`, `-16`, `-20`, `-24`, `-28`, `-32` | the sign sits in its own cell and a one-digit number leaves its tens cell blank. `-32` is the frame mid-roll between -10 and -9, where the number loses a digit |
| `switch-1`, `-2`, `-3` | the second readout is SEMITONES at 0, laid out in its field |

The unchanged frames are `settled-210ms`, `settled-m12st`, `settled-m24db`, the nine `roll-time` frames, `roll-semi-40` and the three `muted` frames. Their numbers already filled the field.

4. The field tests build the value rows from the wording of Value field, taking only the knob's readings from the code. They compare every number of every knob in the default layout (about 3,600) cell by cell, then check that nothing lies left of the field and the unit never moves while numbers roll. They also check 99 to 100, -9 to -10, the sign cell, and the key's left-aligned field.
5. The voice has no byte reference, because the proof's voice comes from Adam's recordings. Its tests check the level map, the heights, the scroll, the rim, the walk and three input levels through the whole path.
6. `ctest` writes `build/oled-readout-{210ms,65pct,m12st,m24db,160ms,key}.pgm`, `oled-readout-roll-99-100.pgm`, `oled-readout-roll-m10-m9.pgm`, `oled-muted.pgm`, `oled-bypass.pgm`, `oled-voice-{quiet,mid,loud}.pgm` and `oled-face-menu.pgm` at 4x.
7. To refresh the goldens after the owner changes the proof:
```
clang++ -std=c++17 -O2 -I firmware/cubevox/src -I tools/font-editor/fonts tools/oled-proof/oled_motion.cpp \
  firmware/cubevox/src/core/canvas.cpp firmware/cubevox/src/core/fonts.cpp -o build/oled_motion
build/oled_motion build/oled-motion.raw && python3 -I tools/oled_golden.py build/oled-motion.raw test/firmware/golden
```

## Open decisions
Each item says what is built now. The owner approves text and screens per numbered proof.

1. Level map. The brief gives -6 dBFS for full height and -50 dBFS for zero. It is linear in dB between them, so a quiet voice shows. A map linear in amplitude would show far less.
2. Height curve. The proof renderer raises the level to 0.8 again when it sets the height. The build keeps that, as `kRibbonCurve`. Setting it to 1 drops it.
3. Walk. It covers the voice screen only, as the brief says. MUTED and the readout stay put, and MUTED can sit unchanged for hours. Giving MUTED the same shift is a small change to `renderMuted`.
4. Menu motion. Opening and closing the menu dissolves. The cursor, the values and the sub-screens do not, because the menu stays as it is. They show at the next tick, up to 40 ms after the click.
5. Units. `HZ` is on the proof sheet and no card carries it, since Unison Rate is a percent. `KnobDesc::decimals` is ready for a parameter that declares one.
6. No panel. The face stays off, as under Timing.

## Bench
1. UI cost as load % at 480 MHz with the default chain running, measured with the face on and off. The audio side adds one multiply-add per input sample.
2. A knob turn raises the readout, the digits roll, it fades after 2 s, mute shows MUTED, and the dissolves read as smooth on glass.
3. Whether a loop pass that blocks about 3 ms on a page write slows the knob scan enough to feel.
4. The level map against the real mic and line input: where speech and singing sit between -50 and -6 dBFS.
5. TOP SWITCH set to BYPASS with the real switch shows BYPASS.
6. The 60 s walk, seen once on glass.
