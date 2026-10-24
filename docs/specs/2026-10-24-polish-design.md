# Polish stages and the menu

Owner 2026-10-24. Two always-available stages close the signal path, POLISH COMP then POLISH EQ,
each an ON/OFF row in the menu. The menu gets its final order and the top-left switch gets a
MUTE/BYPASS assignment. The menu stays minimal: one row per item, no sub-pages.

## Signal path tail
```
slot 1 … slot 8 → POLISH COMP → POLISH EQ → output trim (LINE/INSTRUMENT) → DAC
```
1. Both stages run always; OFF is a settled bit-exact passthrough reached through the same
   50 ms crossfade the EQ already uses, so a click on the menu never clicks in the audio.
2. Level rule: each stage at its default (ON) outputs 0 to +0.5 dB RMS over its input on program
   material. The trim that gets it there lives in the stage tuning (`trimDb`); `level_test` enforces
   it with one row per stage.
3. TOP SWITCH = BYPASS: with the switch on, the output is the dry input (post input gain, pre
   chain) through a 50 ms equal-power crossfade; the chain and both polish stages keep running so
   the switch back is seamless. TOP SWITCH = MUTE: as today, the DAC soft-mute sequence.

## Menu, final order (8 rows, 5x7, no title row, cursor `>` in column 0)
| Row | Text | Click |
|---|---|---|
| 1 | `POLISH COMP ON` | toggles ON / OFF |
| 2 | `POLISH EQ ON` | toggles ON / OFF |
| 3 | `OUT LINE` | cycles LINE / INSTRUMENT |
| 4 | `INPUT MIC` | cycles MIC / PEDAL |
| 5 | `TOP SWITCH MUTE` | cycles MUTE / BYPASS |
| 6 | `CHAIN ORDER` | opens the read-only chain view |
| 7 | `UPDATE FIRMWARE` | opens the confirm screen |
| 8 | `EXIT` | closes the menu |

Turn moves the cursor; the list does not scroll. Every change saves through the layout record's
existing debounce. Screens are approved per numbered proof, as always.

## POLISH COMP, a gentle live vocal compressor
Feed-forward, mono, no lookahead, log-domain gain computer, in `engine/polish_comp.h`.

| Tuning | Value | Why |
|---|---|---|
| Detector | RMS, 10 ms window, sidechain 2-pole high-pass at 150 Hz | plosives and handling noise never pump the gain |
| Threshold | -18 dBFS RMS | sits just under a normal singing level at the chain's unity |
| Ratio | 2.2 : 1 | levelling, not squashing |
| Knee | 6 dB soft | no audible onset |
| Attack | 10 ms | consonants pass |
| Release | 150 ms, program-independent | steady on held notes, no breathing |
| Max gain reduction | 12 dB | a shout stays a shout |
| Makeup | `trimDb` in the tuning, set by `level_test` | unity at default |

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
object, optional on input with the same defaults, always present in replies.

## Emulator and render
`cubevox-proto` and `cubevox-render` run the same tail (comp, eq, trim) so loops audition exactly
what the box plays; `--polish-comp off` and `--polish-eq off` switch them for A/B listening.

## Tests
1. `test/polish_comp_test.cpp`: static curve (gain reduction at -30, -18, -12, -6, 0 dBFS inputs
   within 0.3 dB of the knee formula), attack and release times within 20 %, sidechain high-pass
   (a 50 Hz tone at -6 dBFS causes under 0.5 dB of reduction), OFF is bit-exact after the
   crossfade, no NaN or denormal on silence.
2. `level_test` rows: `polish COMP defaults`, `polish EQ defaults` (exists).
3. `test/firmware/core_record.cpp`: v2 round trip, v1 record decodes with defaults, a bad
   settings byte is rejected.
4. `test/firmware/core_protocol.cpp`: the three fields in and out, absent on input means default.
5. `test/firmware/ui_menu.cpp`: the eight rows in order, each click, the saved settings, proofs
   `oled-menu.pgm` for the owner.
6. Bench (hardware, later): bypass crossfade audible-click free on glass, comp cost in load %,
   TOP SWITCH both ways with the real switch.

## Open items for the owner
1. Approve the menu proof by number.
2. Listen to `cubevox-render` with the comp on and off on the voice loop; the threshold and ratio
   are the two numbers to move if it reads as too much or too little.
