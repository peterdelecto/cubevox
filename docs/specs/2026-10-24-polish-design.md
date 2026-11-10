# Polish stages and the menu

Owner 2026-10-24. Two always-available stages close the signal path, POLISH COMP then POLISH EQ,
each an ON/OFF row in the menu. The menu gets its final order and the top-left switch gets a
MUTE/BYPASS assignment. The menu stays minimal: one row per item, no sub-pages.

## Signal path tail
```
slot 1 … slot 8 → POLISH COMP → POLISH EQ → soft ceiling → output trim (OUTPUT PAD) → DAC
```
1. Both stages run always; OFF is a settled bit-exact passthrough reached through the shared
   20 ms one-pole crossfade (`smooth::kSmoothCoef`), so a click on the menu never clicks in the audio.
2. Level rule: each stage at its default (ON) outputs 0 to +0.5 dB RMS over its input on program
   material. The trim that gets it there lives in the stage tuning (`trimDb`); `level_test` enforces
   it with one row per stage.
3. TOP SWITCH = BYPASS: with the switch on, the output is the dry input (post input gain, pre
   chain) through a 50 ms equal-power crossfade; the chain and both polish stages keep running so
   the switch back is seamless. TOP SWITCH = MUTE: as today, the DAC soft-mute sequence.
4. A change of the output trim ramps across one 64-sample block (1.3 ms), so OUTPUT PAD OFF / ON
   never steps 30 dB between two samples.
5. A block with a NaN or an infinity out of the slots is zeroed before POLISH COMP, so the comp and
   EQ state stay finite. The output cuts to zero, every engine clears in the silent blocks that
   follow, and the output fades back in; `status` counts it as `nan_blocks`.

## Menu, final order (14 rows, 4 visible, Myrtle 7X12, no title row, 16 px row pitch)
Owner 2026-10-30, proof A3 of `tools/oled-proof/menu-font-proof-2.png` ("the menu item text is too small", then "these are quite crammed" of the 12 and 13 px pitches): the list draws in the 7X12 font. Each row is a label at the left edge and a value right-aligned to the right edge, no colon.

The order (owner 2026-10-30, "looks good") runs from the input to the output and ends with the rows touched once. The HPF and LPF rows left with the 2026-10-31 POLISH EQ spec; the SLOT rows came on 2026-11-01 (below and `2026-11-01-slot-7-choices-design.md`); BRIGHTNESS came on 2026-11-02 (`2026-10-24-oled-gui-design.md`, "Brightness and dimming"). Owner 2026-11-05: "INPUT -> INPUT PAD. change REVERB to SLOT 8 and allow selection of chasm or spring reverb. put it in order like the other SLOT selections. OUTPUT -> OUTPUT PAD. BRIGHTNESS -> OLED BRIGHT. add a function to hold down the menu encoder button to exit the menu". The rows below are the result: the reverb row moved from row 2 to sit with the other SLOT rows in slot order, and the two jack rows and the brightness row took new names.

| Row | Label | Value | Click |
|---|---|---|---|
| 1 | `INPUT PAD` | `OFF` | cycles OFF / ON (was `INPUT`, MIC / LINE). ON is the old LINE: the 25.7 dB pad of relay K103 is in circuit (hardware spec row 15) |
| 2 | `SLOT 3` | `OCTAVE` | cycles OCTAVE, FORMANT, APRAXIA in the slot that holds one of them |
| 3 | `SLOT 4` | `UNISON` | swaps UNISON / MOB, below |
| 4 | `SLOT 6` | `DISTORTION` | swaps DISTORTION / DEEPTHROAT in the slot that holds either |
| 5 | `SLOT 7` | `GATE` | cycles GATE, LO-FI, CHATTER, passing over a layout over the CPU lines |
| 6 | `SLOT 8` | `SPRING` | swaps SPRING / CHASM (was `REVERB`, row 2; owner 2026-10-30 for the swap). The label is the number of the slot that holds the reverb card. The value is that card; a click swaps the slot's card and saves the layout record through the path a layout from the app takes, so the audio fades, resets and comes back. The row never reads NONE (owner 2026-10-30): every valid layout holds exactly one reverb card, so a record without one is replaced by the default layout at boot like any other invalid record |
| 7 | `POLISH COMP` | `ON` | cycles ON / OFF |
| 8 | `VOX EQ` (was `POLISH EQ`, ON/OFF, default ON; superseded by `2026-11-05-vox-eq-design.md`) | `OFF` | cycles OFF / ON / ON+CUTS |
| 9 | `OUTPUT PAD` | `OFF` | cycles OFF / ON (was `OUTPUT`, LINE / MIC). ON is the old MIC: a digital -30 dB trim before the DAC that puts the output near the level of the mic the box received, so a mixer mic channel needs no gain change when the box is inserted (owner 2026-10-24) |
| 10 | `TOP SWITCH` | `MUTE` | cycles MUTE / BYPASS |
| 11 | `OLED BRIGHT` | `HIGH` | cycles LOW, MID, HIGH, MAX, the screen's normal brightness (was `BRIGHTNESS`; owner 2026-11-02) |
| 12 | `DEBUG` | `OFF` | cycles OFF / ON (owner 2026-10-30). ON draws the audio load figure on the voice screen, OLED GUI spec, Voice item 9 |
| 13 | `UPDATE FIRMWARE` | `>` | opens the confirm screen |
| 14 | `EXIT` | | closes the menu |

A SLOT row's label is the number of the slot that holds its cards, and `SLOT -` when no slot does. The five SLOT rows run in slot order, 3, 4, 6, 7, 8, so the default layout reads them in that order.

The pad rows are named after the pads. Their value says whether the pad is in the path: INPUT PAD ON is relay K103's pad in circuit, OUTPUT PAD ON is the -30 dB output trim. The stored settings (`inputJack`, `outputLevel`), the record bytes and the protocol strings `mic` and `line` keep their values; only the text on the glass changed.

### The user's walk
1. Adam turns the MENU knob from the face and clicks. The list opens on INPUT PAD with four rows showing. He turns and the rows scroll in the order above; the SLOT rows sit together between the pad and the polish rows.
2. A click on a row changes it at once and the value on the right changes. A click on a SLOT row swaps that slot's card, so the readout and the audio change after the layout fade. A click on UPDATE FIRMWARE opens the confirm screen, where a click starts the update and a turn cancels. A click on EXIT closes the menu.
3. Holding the knob down for 600 ms (`kMenuHoldMs`) from any row closes the menu, the same as the EXIT row, and changes nothing on the way. The press does nothing until it ends or reaches 600 ms, so the row under the cursor is not clicked by a hold. The face comes back when the 600 ms is up, with the button still down; letting go then does nothing.
4. A click now acts when the button comes up, not when it goes down, because the box cannot tell a click from a hold until the press ends or reaches 600 ms.

What can go wrong:
1. A hold that ends just under 600 ms (599 ms) reads as a click and acts on the row under the cursor. The menu closes only once the full 600 ms has passed.
2. A hold on the update confirm screen does nothing: EXIT is not on that screen, a click there starts the update, and a long press must not start it. A turn cancels, as before. The starting screen takes no input, as before. The chain view, which no row opens, ignores a hold too.
3. A hold with the menu closed opens the list, once the 600 ms is up, as a click does.
4. Holding MENU at power-on belongs to the bootloader (`2026-10-24-field-update-design.md`) and is not this. A button still down when the firmware starts reports nothing when it is let go.
5. The press itself, a hold and a click each count as activity for the 15 s menu timeout and for the screen dimming. So a press that starts just before the timeout keeps the list open: the click acts on its row, and a hold closes the list and does not open it again.

Detection lives in `core/menu_button.h` (`MenuButton`), fed the debounced level by `board/input.cpp` and by the emulator's window; the time counts from the debounced press. `UiInput` carries `click` (a release before 600 ms), `hold` (sent once, at 600 ms) and `press` (the debounced down edge, sent once per press, never for a button already down at power-on).

Owner 2026-11-01, "crowd and unison will be interchangeable in the menu like the two reverb modes
are": one more row sits right after SLOT 3. Its label is the number of the slot that holds Unison
or the Mob card, `SLOT 4` in the default layout, `SLOT -` when no slot holds either. Its value is
`UNISON` or `MOB`, the card the layout holds. A click swaps that slot's card through the same path
as the SLOT 8 row, so the layout saves and the audio fades, resets and comes back, and it keeps the
old card when the new one would put the layout over the CPU lines. The Mob row measured on the
board 2026-11-01 (13 % mean, 15 % worst) puts the default layout with Mob at 72 % mean and 82 %
worst, under the lines; until that row lands in `costs.h` the old estimate of 19 % holds the swap
back at 78 %, and the click leaves Unison in place. The Mob card is the
registry's `crowd` id under its display name Mob (`2026-10-31-crowd-design.md`). Built v2.00.82,
proofs `build/proofs/menu/01-menu-slot4-unison.png` and `02-menu-slot4-mob.png`.

The owner's `[A \ B]` notation names the value set. A row shows the current value and a click
cycles it. The list has no CHAIN ORDER row (owner 2026-10-24, no-op for now). `drawChainView` and
its proof stay in the firmware, and no row opens the view. Turn moves the cursor. Four rows show
at a time, so the list is three pages (owner 2026-10-30, "its ok to have 2 or 3 pages"); when the
cursor moves past the top or bottom of the window the list scrolls by one row, only as far as the
cursor needs, and there is no scroll mark. The window starts at row 1 each time the menu opens, and
a cancelled update confirm returns to the same cursor and window. Every change saves through the
layout record's existing debounce. Screens are approved per numbered proof, as always.

The list rows sit on a 16 px pitch, row r from pixel line 16r. The 12 px text starts at y = 16r + 1,
which is y = 1, 17, 33 and 49. The label starts at x = 1 and the value ends at x = 126, measured in
the font's own widths at its 2 px ink gap (a long card name in the readout title bar uses the same
font). The cursor row is inverted full width over 14 px, from 16r to 16r + 13, so it keeps 1 px
clear above and below the ink (owner rule of 2026-10-12) and leaves 2 px of dark between one bar
and the next row's; there is no `>` in front of the cursor item. Every full label fits beside its
widest value with 7 px to spare on the tightest row. The 7X12 font gains three
glyphs for the list, `>`, `k` and `z`, drafted for the owner to refine in the font editor as the
flat sign was; nothing else in the font changes. The confirm, chain, idle and update screens keep
the 5x7 font on the 8 px grid.

## POLISH COMP, a gentle live vocal compressor
Feed-forward, mono, no lookahead, log-domain gain computer, in `engine/polish_comp.h`.

| Tuning | Value | Why |
|---|---|---|
| Detector | RMS, 10 ms window, sidechain 4-pole high-pass at 150 Hz (38 dB down at 50 Hz) | plosives and handling noise never pump the gain; a 50 Hz tone at -6 dBFS causes no reduction at any tuning |
| Release | program-independent | steady on held notes, no breathing |
| Makeup | `trimDb` per tuning, set by `level_test` on the program scaled to -23 dBFS RMS | unity at the level the voice actually has; the other stages are linear and keep the test's -18 dBFS program |

The engine holds one tuning, FIRM (owner 2026-10-24, by ear on the loops against GENTLE and MEDIUM,
which are no longer in the code). Adam's loops sit near -23 dBFS RMS, which is why the threshold sits
where it does.

| Tuning | Threshold | Ratio | Knee | Attack | Release | Max GR | Makeup | On the loops |
|---|---|---|---|---|---|---|---|---|
| FIRM | -30 dBFS | 4 : 1 | 4 dB | 3 ms | 100 ms | 18 dB | +5.85 dB | about 3.5 dB average, 9 to 12 dB peaks |

`cubevox-render --polish-comp firm|off` renders it on and off for listening.

Gain per sample from the envelope through the engine's fast log2/exp2. OFF settles to a
bit-exact copy. CPU cost is measured on the bench and added to the chain table; the budget rule
(75 % / 95 %) of the CPU pass 2 spec applies.

## Soft ceiling
Makeup plus a transient that passes the 3 ms attack can exceed full scale, so `engine/soft_ceiling.h`
closes the tail ahead of the output trim (`2026-10-30-output-safety-design.md`, section 3): bit-exact below -2 dBFS, a tanh shoulder above it with
matched value, slope and curvature, never past 1.0, NaN mapped to 0. A +6 dBFS peak leaves at
0.9999965. None of the nine listening renders touches it. Estimated cost about 0.05 % of the
480 MHz budget below the knee and 0.6 % while a sample is in the shoulder; measured on the bench.

## POLISH EQ
The existing `engine/polish.h` stage with its fixed voicing (110 Hz high-pass, -2.5 dB at 300 Hz,
+1.5 dB presence at 3.5 kHz, +1.5 dB air shelf). The menu row drives `PolishParams::on`; nothing
else in the voicing is reachable from the box.

## Settings and the layout record
`Settings` gains `polishComp` (bool, default on), `polishEq` (bool, default on), `topSwitch`
(`Mute`|`Bypass`, default `Mute`). The record schema becomes v2 with three bytes after the two
existing settings bytes. A v1 record decodes with the defaults for the new fields and is written
back as v2 on the next save; v2 records are refused by a v1 firmware as today (schema mismatch
falls back to the default layout). The serial protocol carries the three fields in the settings
object, optional on input where an absent field leaves the box's value unchanged, always present
in replies. The output level and input jack bytes keep their values (output 0 LINE, 1 MIC; jack
0 MIC, 1 LINE). The menu shows them as the INPUT PAD and OUTPUT PAD rows (below); the stored
values, the record bytes and the protocol strings `mic` and `line` do not change.

## Emulator and render
`cubevox-proto` and `cubevox-render` run the same tail (comp, eq, trim) so loops audition exactly
what the box plays; `--polish-comp off` and `--polish-eq off` switch them for A/B listening.

## Tests
1. `test/polish_comp_test.cpp`: static curve (gain reduction at fixed offsets around the
   threshold within 0.3 dB of the knee formula), attack and release times within 20 %, sidechain high-pass
   (a 50 Hz tone at -6 dBFS causes under 0.5 dB of reduction), OFF is bit-exact after the
   crossfade, no NaN or denormal on silence.
2. `level_test` rows: `polish COMP defaults` (FIRM), `polish EQ
   defaults` (exists). `test/soft_ceiling_test.cpp`: a +6 dB transient stays under full scale,
   a -3 dBFS sine is bit-exact, no NaN out for any input.
3. `test/firmware/core_record.cpp`: v2 round trip, v1 record decodes with defaults, a bad
   settings byte is rejected.
4. `test/firmware/core_protocol.cpp`: the three fields in and out, absent on input means unchanged.
5. `test/firmware/ui_menu.cpp`: the 14 rows in order on the 16 px pitch, the scroll window, the
   other screens on the 8 px grid, each click, the saved settings, the 600 ms hold that closes the
   list, the 15 s timeout, a press that starts before the timeout and ends after it (the click acts
   on its row, the hold closes the list), proofs `oled-menu.pgm` for the owner.
6. Bench (hardware, later): bypass crossfade audible-click free on glass, comp cost in load %,
   TOP SWITCH both ways with the real switch.

## Open items for the owner
1. Approve the menu proof by number.
2. Listen to `cubevox-render` with the comp on and off on the voice loop; the threshold and ratio
   are the two numbers to move if it reads as too much or too little.
