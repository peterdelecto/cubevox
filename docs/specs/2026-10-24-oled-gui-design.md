# OLED GUI, runtime screens

Owner brief 2026-10-24, with the foreman's decided answers of 16:05. The box shows three runtime
screens (voice, readout, MUTED) and the menu. Every change between screens is one 2-frame dissolve.
The pixel reference is `tools/oled-proof/oled_motion.cpp`, which draws on the firmware's own canvas
and fonts. The menu is `2026-10-24-polish-design.md` and does not change.

## Status
1. Host part built and tested 2026-10-24. The readout and MUTED frames equal the proof renderer byte for byte.
2. The owner approved proofs 3 to 10 on 2026-10-24.
3. A sweep test measures every parameter of every card for sideways movement and is a hard gate, as set out under Tests and proofs.
4. The Readout section stands. `2026-10-29-readout-card-design.md` (proof D) was set aside by the owner on 2026-10-30 before it was built; the readout keeps its three rows. The same day the owner asked for an on-screen sign when a slot's toggle is flipped, set out under Toggle.
5. Bench part open. The list is in Bench, below.
6. The application drives the panel through `board/sh1106.c`, the driver the bootloader compiles too, with no U8g2 or Wire. Panel, below, sets out how it starts, retries and recovers.

## Screens
| Screen | Shows | Rows |
|---|---|---|
| Voice | (superseded 2026-11-06 by `2026-11-06-scope-design.md`: the output is a scope trace, not a ribbon) the output as a ribbon scrolling right to left, each tick's amplitude over the recent peak, steady noise under a tracked floor drawn as the line, with the input level as a boxed segment meter at the left and the DEBUG load figure bottom right | ribbon columns 13 to 126, horizon at row 32, up to 22 px each side, 1 px rim; meter box columns 5 to 20, rows 8 to 56, 16 segments inside (owner 2026-11-01) |
| Readout | the knob that moved last | title bar rows 0 to 13 with the slot and card name black in 7X12 centred, name 9X12 top at 16, value 16X24 top at 34 |
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
8. A readout belongs to the card its knob was on. When a new layout puts another card in that slot, the readout leaves at the next tick.
9. The menu takes no input until the face's first tick. A click or a turn during the boot splash, when nobody could see the menu, does nothing. Without a panel the face never ticks, so the menu never listens. MENU held at power-on still reaches update mode, because the bootloader reads it.
10. The splash covers the power-on wait (owner 2026-11-10). It stays up for at least `kSplashMs` and until `mutePowerOnWait()` clears, so MUTED never shows while the box only waits to close the relay. MUTED shows only for the MUTE toggle or a fault. A box that is held, tripped or latched shows MUTED once the 6 s wait has passed.

## Readout
1. Row one is a title bar (owner 2026-10-30, proof C2 of `tools/oled-proof/readout-title-proof.png`): the full width lit over rows 0 to 13, with the slot number, a space and the card name in capitals, Myrtle 7X12 spaced by ink with a 2 px gap, black, centred on the width, glyph top at row 1, so 1 px of bar shows above and below the letters. The bar is 12 + 2 × kScreenPad tall. The longest default title, `8 SPRING REVERB`, is 108 px wide; every card name in the registry fits with bar on both sides.
2. Row two is the parameter name, 9X12, centred, top at row 16.
3. Row three is the value, a space and the unit, 16X24, with its top at row 34, in a 24 px band that spans rows 34 to 57. The unit is written as in print, `st`, `ms`, `dB`, `%` (owner 2026-10-30; the 16X24 font carries lowercase d, m, s and t for it, Value formatting 3). The row is centred on what it draws, set out under Value field.
4. A text value (the autotune key) has no unit and does not roll. It is centred on its own width (owner 2026-10-30).
5. The value eases to the knob with a 20 ms time constant, which closes 86.5 % of the gap per tick.
6. Each tick the value sits between two whole numbers. A cell that differs between them rolls up out of its place while the next glyph rolls in from below. Both are clipped to the band.
7. The roll offset is `round(fraction * 24)` px. A cell that is equal in both numbers draws plain.
8. A new readout starts at the knob's reading. A different knob, or a readout that had gone, never eases from the old value.

## Toggle
Owner 2026-10-30, board 1 on the bench: a flipped slot toggle needs a sign on the screen, as the
MUTED word is for the top switch.
1. A flip of a slot's toggle raises the readout for that slot as a knob move does, with the same
   hold, priority, dissolve and card rule (Priority 2, 4, 7, 8). A flip under the open menu is
   not shown; a flip on a slot with no card raises nothing.
2. The toggle readout keeps the title bar, leaves row two empty, and draws ON or OFF on the value row,
   16X24, centred, top at row 34. The words are ON and OFF, never BYPASS, so the slot's state
   never reads like the top switch's BYPASS screen.
3. A slot has three readouts, one for each knob and one for its toggle. A second flip of the
   toggle already shown, like a turn of the knob already shown, refreshes the readout in place,
   without a dissolve. A move to another of the slot's three readouts, or to another slot,
   dissolves (Dissolve 6; owner 2026-11-10, either way is fine, so the box stays as built).
4. The panel hands the face one flip at a time with the slot and the new state, debounced by the
   20 ms switch debounce. The first debounced level after power-on is the baseline and raises
   nothing.
5. Goldens: the on and off frames join the golden set through `oled_motion.cpp` and
   `tools/oled_golden.py`; the sweep gate is unchanged.

## Value field
Owner 2026-10-30: the value row is centred on what it draws. The field holds the cells of the wider of the two numbers the roll sits between, the space and the unit, and the field is centred on the panel. A settled value is one number, so its field is that number's own width. Nothing moves while two numbers of the same digit count roll. The row shifts by half a cell exactly when a digit or the sign appears or vanishes, at the start of a roll that gains a digit and at the end of one that loses it.

1. A numeric field is the sign cell when it is drawn, the digit cells, a space and the unit.
2. A cell is 18 px wide, a 16 px glyph cell and 2 px of spacing, and a glyph's ink is centred in its cell.
3. The digit cells are as many as the digits of the wider of the two numbers. The digits are right-aligned, so the shorter number leaves its leading cell blank.
4. The sign cell is drawn when either number carries a sign, `-` or `+` (Value formatting 1 and 2). It comes first, and a number without a sign leaves it blank. A settled 0 on a signed knob has no sign cell.
5. The field's width is `cells * 18 - 2` px, plus 2 px and the width of the space and the unit when the knob has a unit. Its left edge is `(128 - width) / 2`, rounded down. The space and the unit start at the left edge plus `cells * 18`.
6. A number that gains a cell moves its first cell 9 px left and its unit 9 px right, and one that loses a cell moves them back. On a percent knob, 99 to 100 widens the field from 2 cells to 3 at the start of the roll: the first cell moves from x 34 to x 25, the unit from x 70 to x 79, and a `1` rolls into the new leading cell while each 9 rolls to 0 in its own cell. `+5 st` settled is the sign cell and one digit cell, 68 px wide from x 30.
7. A text value (the key) has no cells. Readout item 4 centres it on its own width.

## Value formatting
All of it comes from the card's parameter table in `core/registry`, never from the screens.

| Parameter | Rule | Example |
|---|---|---|
| Continuous | rounded whole number of the unit | `210 ms`, `65 %`, `-24 dB` |
| Stepped number | the step's whole number | `+5 st`, `0 st`, `-12 st` |
| Text | the name at the step | `F / Dm`, `D♭ / B♭m`, `MOB` |

1. A positive number carries `+` only when the parameter's range reaches below zero.
2. Zero carries no sign.
3. The units are `st`, `ms`, `dB` and `%`, as written in print. The 9X12 and 16X24 fonts ink lowercase b, d, m, s and t, and the 7X12 font b, d, k, m, s, t and z; the 5x7 font inks no lowercase. A lowercase letter a font does not ink draws as its capital. The card name in the title bar and the parameter name go to capitals before they are drawn, so on the readout only the value row shows lowercase.
4. The key names write a flat as a lowercase b (`Db / Bbm`). The 9X12 and 16X24 fonts ink a flat sign in the b cell, and the m of a minor key draws as the font's own lowercase m, so the readout shows `D♭ / B♭m`. In the 5x7 font a b draws as B.
5. `knobCells` gives the cells of a parameter and `formatCells` fills them.
6. `core_registry.cpp` checks that every knob's field fits the panel in 16X24 with 1 px each side. The widest numeric field is four cells and ` ms`, 109 px (Input gate DECAY, Gate DECAY and Chatter LENGTH). The widest name is `F# / D#m` at 116 px. The flat is 12 px wide, 1 px narrower than the 13 px sharp, so the flat names stay inside that width (`Db / Bbm` is 114 px).

## Voice
1. Superseded in part 2026-11-06 and 2026-11-07 by `2026-11-06-scope-design.md`: the scope reads the chain output ahead of POLISH COMP from its ring, and no output energy is taken. The input half stands. The audio interrupt sums the squares of the input samples of every block where they enter the chain (so the bench clip moves the bar too). The main loop takes the sum with its sample count at each UI tick, in a masked take.
2. Superseded 2026-11-07 by `2026-11-06-scope-design.md` section 3: the window top, the amplitude map and `level::OutputMeter` are gone; the scope's scale fits each frame's own peak. The ribbon follows the output and scales to its own recent peak (owner 2026-10-30). A window top `t` in dBFS tracks the tick RMS `d`: when `d` is louder than `t`, `t` becomes `d` at once; otherwise `t` falls 10 dB per second of real time and never under -30 dBFS. The ribbon level is the tick's amplitude over the peak's, `10^((d - t) / 20)`, held to 0..1, so the loudest recent tick reaches the top, a tick 6 dB under it reaches half, and one 20 dB under reaches a tenth. The map is linear in amplitude, the way a waveform overview draws (owner 2026-10-30, after the dB map with its 0.6 and 0.8 curves put everything within 20 dB of the peak in the top 6 px and drew a block in place of the peaks and valleys). Reviewed again 2026-11-01 ("still scaled too large, doesn't show dips and peaks and valleys"): nine maps were rendered from the last 2.3 s of Adam's 5.5 s clip, R0 this map, R1 to R5 linear in dB over 24, 18, 12, 9 and 6 dB under the top, R6 against a 400 ms running average over 12 dB, R7 the tick's peak sample under the top over 9 dB, R8 R6 with R7's peak; the steeper and average maps cleaned the gaps and tails but a held word stays within 3 dB of its own peak, so its top stayed a block in every map. The owner picked R0, so this map stands. Pictures in `build/proofs/ribbon/`, renderer `tools/ribbon_proof.cpp` on the set-aside branch.
3. Superseded 2026-11-07 by `2026-11-06-scope-design.md` section 3: silence is a frame whose peak is under -54 dBFS, and the floor rule below is gone, because a decaying echo tail is a run of new quietest ticks and the rule drew the whole tail as the line. A floor `f` in dBFS tracks the quietest recent tick, so steady noise draws the line and not a voice (owner 2026-10-30, a Korg Volca idling on the input drew a block of its hiss). When `d` is quieter than `f`, `f` becomes `d` at once, but never under -60 dBFS; otherwise `f` rises 3 dB per second of real time, but never over `t - 12` dB, so the loudest recent tick always draws. A tick under `f + 6` dB is silence, level 0. The ribbon has no release: each 40 ms tick stands on its own, so the gaps between words show as valleys.
4. Superseded 2026-11-06 by `2026-11-06-scope-design.md` (the scope's 103 columns of 8 samples). A tick turns the level into a half-height of `round(level * 22)` px. Each height spans 2 columns, the newest at the right edge, so the ribbon scrolls 2 px a tick, which is 50 px/s. The ribbon runs from column 13 to column 126, 57 levels; column 127 and columns 10 to 12 stay blank (owner 2026-10-30, more black around the meter).
5. Superseded 2026-11-06 by `2026-11-06-scope-design.md` (a 1 px trace, no rim dither). A column is solid from `32 - h` to `32 + h`. A 1 px rim above and below it is lit where the Bayer threshold of the pixel is under 32, which is half of them.
6. At height 0 the column is the one pixel on row 32. In silence the screen is the line, the empty meter box and nothing else.
7. Superseded 2026-11-06 by `2026-11-06-scope-design.md`: the scope has no history and reads the ring only while the voice screen shows; no meter tracks the output under the other screens. The ribbon keeps filling while another screen shows, so the voice returns with its history.
8. The input meter (owner 2026-10-30, "a minimal, thin input level VU meter on the left", then "a bounding box, like a thermometer without the bulb, in segments", then "5 px wider with more black around it"): a 1 px outline box with its sides at columns 5 and 20 and its top and bottom at rows 8 and 56, always drawn on the voice screen so the meter is present in silence (owner 2026-11-01, "twice as wide, 4 px from the edge"; columns 21 to 23 stay blank before the scope). Inside, 16 segments across columns 6 to 19, each 2 rows tall with 1 row of gap, segment 1 at rows 54 and 55 up to segment 16 at rows 9 and 10. Lit segments = `round(level * 16)`, the level linear in dB of the input tick RMS from -50 dBFS (0) to 0 dBFS (16). The level rises at once and falls with a 120 ms time constant, counted in real time so a late tick falls further, and a level under 0.01 snaps to 0. Each segment is about 3 dB and the top one is the clip light. The box, the segments and the scope trace walk together, and the meter shows on the voice screen only.
9. The DEBUG load figure (owner 2026-10-30, the polish spec's DEBUG menu row): with DEBUG on, the voice screen draws the worst audio block of the last whole second as a percent of the block budget (640000 cycles at 480 MHz) in 5x7, last column 126 and top row 56, so `42%` keeps 1 px from the right and bottom edges. It reads 0 for the first second after boot, is held to 0..999, refreshes each UI tick, does not walk, and never appears on the readout, the toggle readout, MUTED, BYPASS or the menu.

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
2. Superseded in part 2026-11-06 by `2026-11-06-scope-design.md`: there is no rim dither. The shift moves the finished picture, so the rim dither moves with the ribbon. There is no animation.
3. The count starts at the first tick and survives the millisecond counter wrapping.

## Brightness and dimming
Owner 2026-11-02: "lets dim the screen. we also need a menu item that adjusts screens normal brightness."
1. The menu row OLED BRIGHT (was BRIGHTNESS; renamed by the owner 2026-11-05) sets the normal brightness: LOW, MID, HIGH or MAX, panel contrast 48, 128, 207 or 255. HIGH is the default and the contrast every screen had before. The setting is saved in settings byte 8 of the layout record and does not go over USB.
2. The screen dims to contrast 8 after 10 minutes with no knob move, slot toggle flip, top switch flip, MENU turn or press, and no voice. Voice is input above −50 dBFS over one UI tick, the input meter's floor. A noisy room that keeps the input above that never dims the screen.
3. Any of those brings the normal brightness straight back, and the control still does its job: a knob turn moves its parameter, a MENU click opens the menu. A MENU hold counts as a touch too (the hold closes an open menu, `2026-10-24-polish-design.md`, "The user's walk").
4. Dimming works on every screen, MUTED and BYPASS included. A dark screen on stage reads as a dead box, so the screen dims and never goes black.
5. The application sends the contrast as its own command between two page writes, and again after a panel restart, which puts the init sequence's 207 back. The saved level applies before the splash's first page. The bootloader's update screens keep 207.
6. The emulator draws a lit pixel at HIGH in the ink it always had. A lower contrast fades it toward the dark glass by contrast / 207, and MAX takes it to white.
7. The MUTED screen does not walk. The owner named only the dimming on 2026-11-02.
8. Built v2.01.14: `core/brightness.h` holds the four levels and the dimmed contrast, `ui/screen_dim.h` the idle rule. The dimmed screen stays dim until the next activity, even if the millisecond counter laps.

## Timing
| Item | Value |
|---|---|
| UI tick | 25 fps, 40 ms |
| Tick gate | 40 ms since the last tick and no dirty page left on the canvas |
| Dissolve | 2 mixed frames |
| Readout hold | 2000 ms after the last move |
| Value easing | 20 ms time constant |
| Input meter release | 120 ms time constant; the ribbon has none (superseded 2026-11-06: the scope redraws every tick and has no release) |
| Walk step | 60 s |
| Dim after | 600 s with no activity |
| Panel flush | one dirty page per loop pass, about 25 ms for a full frame at 400 kHz |

1. The gate keeps a tick from outrunning the flush, which would tear the picture. A write that fails leaves its page dirty, so ticks wait until it goes through.
2. Without a panel the pages stay dirty, so the face does not tick and costs nothing.

## Panel
1. `board/sh1106.c` is the one SH1106 driver. The bootloader compiles it through its `board.mk` and the application through the sketch. Each program passes its own bus: the pins, the alternate function, the I2C and the TIMINGR for its clock. The init sequence is the one U8x8 sends this panel, and the 128 columns start at column 2.
2. Every wait polls a bounded number of times, a few milliseconds. A NACK or a timeout ends the transfer, turns the I2C off and returns false.
3. At boot the application gives the panel 5 starts, 20 ms apart. A start runs up to nine SCL clocks and a STOP, which frees a bus that a reset left mid-transfer, then the init sequence.
4. One dirty page goes out per loop pass, and a failed page stays dirty. The first failure restarts the panel on the next pass. A failure after that, before the panel has taken a whole frame again, waits for a 1 s retry window. A missing panel costs one short start a second, and a panel that appears later shows within a second.
5. After a start the whole canvas goes out again, and the display turns on in the pass that sends the frame's last page.
6. A run of failures prints `[WARN] OLED not answering, retrying once a second` once. Once the panel has taken a whole frame again it prints `[oled] OLED answering again after N failed tries`. `PanelLink` in `core/display.h` holds the rule as pure code with host tests.
7. The application's TIMINGR gives 400 kHz from a 120 MHz I2C2 kernel clock, the PCLK1 of the 480 MHz profile. The 360 MHz profile's 90 MHz only slows the bus. A faster clock, or I2C2 on another source, prints an `[ERROR]` line and the panel stays off.

## Modules
| Module | Owns |
|---|---|
| `core/level` | sum of squares, RMS, dB to level, the meter |
| `core/canvas` | `setPixel`, clipped digits, `copyFrom`, `dissolve`, `markAllDirty`, the Bayer table and the dissolve masks |
| `core/display` | the `Display` interface and `PanelLink`, which says when the backend sends, restarts the panel or waits |
| `core/registry` | `KnobDesc::labels`, `knobDesc`, `knobShown`, `knobCells`, `formatCells`, `knobLabel` |
| `core/knob_filter` | `KnobBank::takeTouch`, one touch per move |
| `ui/gui` | the state machine: which screen, which dissolve step, the eased value, the walk |
| `ui/screens` | `renderVoice`, `renderReadout`, `renderMuted` (the ribbon history is superseded 2026-11-06: `renderVoice` takes a `ScopeFrame`) |
| `core/scope` | superseded 2026-11-06 addition (`2026-11-06-scope-design.md`): the ring of the chain output, `scopeFrame` and the trigger |
| `ui/face` | the top-level draw: the tick gate, the level meter, the screen pick, the dissolve onto the panel, the readout's card, the menu's `listen` |
| `ui/menu` | `Menu::draw` draws the open menu only, `Menu::open`, `Menu::listen` |
| `board/sh1106.c` | the SH1106 driver the application and the bootloader share |
| `board/display_sh1106` | the application's bus from `pins.h`, the boot starts, the page loop under `PanelLink` |
| `board/controls` | `controlsTakeTouch` |
| `board/audio` | `audioTakeInputEnergy` and `audioScope` (the ring, from `core/scope`, as of 2026-11-06; the output energy take went 2026-11-07) |

## Data flow
```
audio IRQ, per block ── sum of squares ──▶ audioTakeInputEnergy ─┐
board/controls ── touch (slot, knob) ──▶ Face::knobMoved ─────────┤
Settings, mute state, Menu::open ────────────────────────────────┴▶ Face::tick
   ▶ Gui::tick ▶ screen, dissolve step ▶ render into target canvas ▶ dissolve onto the live canvas ▶ flushStep
```

## Tests and proofs
1. `test/firmware/golden/*.bin` hold 31 frames of `oled_motion` (1024 page-layout bytes each). `tools/oled_golden.py` cuts them from a run of the proof renderer.
2. The host tests match them byte for byte: the four settled readouts, nine rolling frames of 120 to 210 ms, eight rolling frames of 0 to -12 st, MUTED and its two dissolve frames, one readout replaced by another, the two toggle frames and one voice frame (`voice-meter-ribbon`, scene 12; superseded 2026-11-06 by `voice-scope` and `voice-scope-loud`, see `2026-11-06-scope-design.md`).
3. The proof renderer's `rollValue` lays out the value row by the Value field rule, and the readout goldens are cut from it. `roll-semi-32` stands at -9.9 on the roll from 0 to -12 st, where the number gains a digit.
4. The field tests build the value rows from the wording of Value field, taking only the knob's readings from the code. They compare every number of every knob in the default layout cell by cell (the test requires more than 2,000 numbers), then check that nothing lies left of the field and the unit never moves while numbers roll. They also check 99 to 100, -9 to -10, the sign cell, and each key name centred on its own width.
5. The lateral sweep is the hard gate for the owner's rule, "I don't want anything bouncing around laterally as values change". `checkReadoutSweep` renders the settled readout at every step of every knob of every card in the registry, Chasm Reverb included.
6. A continuous knob runs through every whole number between the lowest and the highest its 4096 pot readings reach. A stepped knob runs through every position. Today that is 30 parameters, 8,666 settled steps and 43,080 rolling frames, and the test prints the counts on every run.
7. Each frame is measured on its pixels and takes no position from a model of the layout. The digits and the sign are found by matching each glyph's picture. The unit's first and last lit columns are read straight off the frame. Each must equal the column the first frame of the same digit count gave, so a shift of one pixel between numbers of the same digit count fails. Between neighbouring counts of cells the first cell must move 9 px and the unit the rest of the 18 px cell, as Value field item 6 says. A key name must be drawn on its own width with its left and right margins within 1 px.
8. The sweep also requires that the cells and the unit account for every lit pixel of the value rows, that nothing above or below those rows changes, and that every step looks different from the one before.
9. Superseded 2026-11-06 by `2026-11-06-scope-design.md`: the voice goldens are `voice-scope` (frame 79 of scene 12) and `voice-scope-loud` (frame 76), and the voice tests check the scope's trigger, columns and scale instead of the level map, scroll and rim. The voice golden `voice-meter-ribbon` is frame 79 of scene 12, a scripted drive the test repeats tick for tick, because the proof's scene 1 comes from Adam's recordings, which the host cannot replay. The voice tests also check the walk, the bar and three levels through the whole path (the level map, the window, the heights, the scroll and the rim are superseded).
10. `ctest` writes `build/oled-readout-{210ms,65pct,m12st,m24db,160ms,key,key-flat}.pgm`, `oled-readout-roll-99-100.pgm`, `oled-readout-roll-m10-m9.pgm`, `oled-muted.pgm`, `oled-bypass.pgm`, `oled-voice-{quiet,mid,loud}.pgm`, `oled-face-menu.pgm` and `oled-splash.pgm` at 4x.
11. `core_canvas.cpp` drives `PanelLink` with a fake clock: the first failure restarts at once, later ones wait for the 1 s window, Lost and Back come once per run, and the window survives a millis() wrap. `ui_splash.cpp` runs the sketch's order through the splash and checks that a click or a turn during it opens nothing.
12. The CI host job runs `node tools/font-editor/check-header.js`, which fails unless `cubevox_fonts.h` is what the font editor exports from `cubevox-fonts.json`.
13. To refresh the goldens after the owner changes the proof:
```
clang++ -std=c++17 -O2 -I firmware/cubevox/src -I tools/font-editor/fonts tools/oled-proof/oled_motion.cpp \
  firmware/cubevox/src/core/canvas.cpp firmware/cubevox/src/core/fonts.cpp -o build/oled_motion
build/oled_motion build/oled-motion.raw && python3 -I tools/oled_golden.py build/oled-motion.raw test/firmware/golden
```

## Open decisions
Each item says what is built now. The owner approves text and screens per numbered proof.

1. Level map. Settled by `2026-11-06-scope-design.md` section 3: the scope fits each frame's own peak, and the input meter maps -50 to 0 dBFS linearly in dB onto its 16 segments (Voice 8).
2. Height curve. Settled by the same spec: the trace draws each sample at its height on the frame's scale, with no curve.
3. Walk. It covers the voice screen only, as the brief says. MUTED and the readout stay put, and MUTED can sit unchanged for hours. Giving MUTED the same shift is a small change to `renderMuted`.
4. Menu motion. Opening and closing the menu dissolves. The cursor, the values and the sub-screens do not, because the menu stays as it is. They show at the next tick, up to 40 ms after the click.
5. Units. `HZ` is on the proof sheet and no card carries it, since Unison Rate is a percent. The knob table has no decimals field; a parameter that needs one adds it with its own width rule.
6. No panel. The face waits on the dirty pages and the driver tries a start once a second, so a panel plugged in later shows the face within a second.
7. Panel reset under a still screen. A panel that loses power and comes back while nothing changes on glass, such as MUTED for minutes, powers up with its display off and still answers its address. Its later writes go through, so the driver never restarts it, and it stays dark until the box restarts. Built now: nothing catches it. Re-sending the init settings and the display-on command once a second would, for about 0.6 ms of bus time a second. Bench 8 shows first whether it happens.

## Bench
1. UI cost as load % at 480 MHz with the default chain running, measured with the face on and off. The audio side adds one multiply-add per input sample and one store per output sample into the scope ring.
2. A knob turn raises the readout, the digits roll, it fades after 2 s, mute shows MUTED, and the dissolves read as smooth on glass.
3. Whether a loop pass that blocks about 3 ms on a page write slows the knob scan enough to feel.
4. The input meter against the real mic and line input: where speech and singing sit on its -50 to 0 dBFS range.
5. TOP SWITCH set to BYPASS with the real switch shows BYPASS.
6. The 60 s walk, seen once on glass.
7. The panel's connector pulled and pushed back while the voice runs. The face returns within 1 s, and the log shows one `[WARN]` line and one `[oled]` line.
8. The panel's VCC lifted for 50 ms while MUTED shows and nothing else changes. Whether the panel comes back dark, as Open decisions 7 expects.
