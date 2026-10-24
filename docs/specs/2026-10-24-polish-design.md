# Polish stages and the menu

Owner 2026-10-24. Two always-available stages close the signal path, POLISH COMP then POLISH EQ,
each an ON/OFF row in the menu. The menu gets its final order and the top-left switch gets a
MUTE/BYPASS assignment. The menu stays minimal: one row per item, no sub-pages.

## Signal path tail
```
slot 1 … slot 8 → POLISH COMP → POLISH EQ → output trim (LINE/MIC) → DAC
```
1. Both stages run always; OFF is a settled bit-exact passthrough reached through the same
   50 ms crossfade the EQ already uses, so a click on the menu never clicks in the audio.
2. Level rule: each stage at its default (ON) outputs 0 to +0.5 dB RMS over its input on program
   material. The trim that gets it there lives in the stage tuning (`trimDb`); `level_test` enforces
   it with one row per stage.
3. TOP SWITCH = BYPASS: with the switch on, the output is the dry input (post input gain, pre
   chain) through a 50 ms equal-power crossfade; the chain and both polish stages keep running so
   the switch back is seamless. TOP SWITCH = MUTE: as today, the DAC soft-mute sequence.

## Menu, final order (7 rows, 5x7, no title row, cursor `>` in column 0, 9 px row pitch)
| Row | Text | Click |
|---|---|---|
| 1 | `INPUT: MIC` | cycles MIC / LINE. LINE puts the 25.7 dB pad of relay K103 in circuit (hardware spec row 15) |
| 2 | `OUTPUT: LINE` | cycles LINE / MIC. MIC is a digital -30 dB trim before the DAC that puts the output near the level of the mic the box received, so a mixer mic channel needs no gain change when the box is inserted (owner 2026-10-24) |
| 3 | `TOP SWITCH: MUTE` | cycles MUTE / BYPASS |
| 4 | `POLISH COMP: ON` | cycles ON / OFF |
| 5 | `POLISH EQ: ON` | cycles ON / OFF |
| 6 | `UPDATE FIRMWARE >` | opens the confirm screen |
| 7 | `EXIT` | closes the menu |

The owner's `[A \ B]` notation names the value set. A row shows the current value and a click
cycles it, because the longest row with both values does not fit 21 columns. The list has no
CHAIN ORDER row (owner 2026-10-24, no-op for now). `drawChainView` and its proof stay in the
firmware, and no row opens the view. Turn moves the cursor; the list does not scroll. Every
change saves through the layout record's existing debounce. Screens are approved per numbered
proof, as always.

The list rows sit on a 9 px pitch, so row r spans pixel lines 9r to 9r + 8, because seven rows
leave breathing room (owner 2026-10-24). The 7 px text starts at y = 9r + 1, which is y = 1, 10,
19, 28, 37, 46 and 55, so it keeps 1 px clear above and below inside the cursor bar (owner rule
of 2026-10-12, an inverted row is text height + 2 px). The cursor bar spans the 9 px row. The
last pixel line stays blank. The confirm, chain, idle and update screens keep the 8 px grid.

## POLISH COMP, a gentle live vocal compressor
Feed-forward, mono, no lookahead, log-domain gain computer, in `engine/polish_comp.h`.

| Tuning | Value | Why |
|---|---|---|
| Detector | RMS, 10 ms window, sidechain 2-pole high-pass at 150 Hz | plosives and handling noise never pump the gain |
| Threshold | -24 dBFS RMS | Adam's loops sit near -23 dBFS RMS; here the loud syllables get 3 to 5 dB and the average about 1 dB. At -18 the comp idled and only its makeup showed |
| Ratio | 2.2 : 1 | levelling, not squashing |
| Knee | 6 dB soft | no audible onset |
| Attack | 10 ms | consonants pass |
| Release | 150 ms, program-independent | steady on held notes, no breathing |
| Max gain reduction | 12 dB | a shout stays a shout |
| Makeup | `trimDb` in the tuning, set by `level_test` on the program scaled to -23 dBFS RMS | unity at the level the voice actually has; the other stages are linear and keep the test's -18 dBFS program |

Gain per sample from the envelope through the engine's fast log2/exp2. OFF settles to a
bit-exact copy. CPU cost is measured on the bench and added to the chain table; the budget rule
(75 % / 95 %) of the CPU pass 2 spec applies.

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
0 MIC, 1 LINE).

## Emulator and render
`cubevox-proto` and `cubevox-render` run the same tail (comp, eq, trim) so loops audition exactly
what the box plays; `--polish-comp off` and `--polish-eq off` switch them for A/B listening.

## Tests
1. `test/polish_comp_test.cpp`: static curve (gain reduction at fixed offsets around the
   threshold within 0.3 dB of the knee formula), attack and release times within 20 %, sidechain high-pass
   (a 50 Hz tone at -6 dBFS causes under 0.5 dB of reduction), OFF is bit-exact after the
   crossfade, no NaN or denormal on silence.
2. `level_test` rows: `polish COMP defaults`, `polish EQ defaults` (exists).
3. `test/firmware/core_record.cpp`: v2 round trip, v1 record decodes with defaults, a bad
   settings byte is rejected.
4. `test/firmware/core_protocol.cpp`: the three fields in and out, absent on input means unchanged.
5. `test/firmware/ui_menu.cpp`: the seven rows in order at their 9 px y positions, the other
   screens on the 8 px grid, each click, the saved settings, proofs `oled-menu.pgm` for the owner.
6. Bench (hardware, later): bypass crossfade audible-click free on glass, comp cost in load %,
   TOP SWITCH both ways with the real switch.

## Open items for the owner
1. Approve the menu proof by number.
2. Listen to `cubevox-render` with the comp on and off on the voice loop; the threshold and ratio
   are the two numbers to move if it reads as too much or too little.
