# cubevox — VOX EQ: POLISH EQ becomes one row with three settings, default OFF

Owner 2026-11-05, after asking whether the input's high end and low end are cut: "one polish eq with
three settings. change it to VOX EQ and default off". Built 2026-11-05.

## Scope
1. The menu row `POLISH EQ` (ON/OFF, default ON) becomes `VOX EQ` with three settings, default OFF.
2. The stage is the one in `2026-10-31-polish-eq-adam-voice-design.md`. Its voicing, tunings and CPU
   row are unchanged.
3. POLISH COMP is not touched.

## The user's walk
1. Sees. The menu list shows `VOX EQ` between POLISH COMP and OUTPUT. A new box, and every box after
   this update, reads `OFF`. The value field is right-aligned, and `ON+CUTS` clears the label by the
   same 7 px or more the menu test holds for every row.
2. Wants. Adam's voice sounds as it comes in, or polished, or polished and with the room's rumble and
   hiss cut.
3. Does. One click steps OFF, then ON, then ON+CUTS, then OFF again. The value changes on the click and
   the sound follows over about 100 ms (the stage's 20 ms smoother).
4. Hears at OFF. Nothing of the stage runs. The input reaches slot 1 with its full range, and the
   output after POLISH COMP is the comp's alone. Both halves are bit-exact copies.
5. Hears at ON. The de-esser runs ahead of slot 1 and the 300 Hz dip, 3.5 kHz presence and trim run
   after POLISH COMP. Nothing is cut at the ends. The de-esser now reads the sibilant band at full
   level, because the 6 kHz low-pass no longer sits ahead of it, so it cuts about 5.0 dB on the test
   voice where ON+CUTS cuts about 4.5 dB.
6. Hears at ON+CUTS. Everything in ON, plus the 70 Hz high-pass and the 6 kHz low-pass (4th-order
   Butterworth each) ahead of slot 1. This is what the earlier POLISH EQ ON did.
7. Goes wrong, an old record. A record saved by an earlier build holds POLISH EQ in settings byte 3,
   usually ON. That byte is retired and ignored, so the box starts at OFF and Adam's saved choice for
   POLISH COMP, the switch and the rest is kept. He sets VOX EQ once and it saves.
8. Goes wrong, a click. A change of setting crossfades. OFF to ON and back fade the whole stage. ON to
   ON+CUTS and back fade the two cuts through their own 20 ms smoother, so a 40 Hz tone moves from full
   level to 0.11 of it with no step larger than the tone's own (`polish_test` "cuts fade").
9. Goes wrong, the level. The output level moves between settings by the stage's own gain. Measured
   on Adam's three clips with the trim unchanged at +1.3 dB, VOX EQ alone reads +0.41 dB at ON and
   +0.15 dB at ON+CUTS, both inside 0 to +0.5 dB. Through the whole tail (front half, POLISH COMP, back
   half) ON+CUTS reads +0.21 dB and ON reads +0.80 dB, because the comp's own +0.31 dB and the stage's
   +0.41 dB add on uncut clips. The clips range from -0.14 to +1.88 dB at ON. The ON tail reading is
   printed by `level_test` and not judged; each stage alone holds the level rule at both settings.
10. Goes wrong, the bench. At OFF the stress run's VOX EQ bucket reads near zero, so its gate against
    `kPolishEq` passes without measuring the stage. A run that must load the stage sets VOX EQ to
    ON+CUTS first (the protocol's `vox_eq`). The row `kPolishEq` in `core/costs.h` stays the
    ON+CUTS cost.

## Settings and the record
1. `Settings::voxEq` is `VoxEq::Off`, `On` or `Cuts` (0, 1, 2), default Off. `polishEq` is gone.
2. Record byte 3 (POLISH EQ) is retired, written 0 and ignored on read, like bytes 6 and 7. The VOX EQ
   setting takes byte 9, the first reserved byte after brightness. Every older record holds 0 there, which
   reads as OFF. No schema bump is needed. A byte above 2 makes the record invalid, as for the other
   settings.
3. Going back to an earlier firmware reads byte 3 as 0, so POLISH EQ is OFF there.

## Protocol
1. The `settings` key `polish_eq` (true/false) is replaced by `vox_eq`, the string `"off"`, `"on"` or
   `"cuts"`. Replies always carry it. A request that leaves it out leaves the box's value.
2. A request still carrying `polish_eq` has the key skipped like any unknown key.
3. The Mac app does not read or write the settings object, so it needed no change beyond a comment.

## Engine
1. `PolishParams` gains `cuts` (default true, so the engine's own default is the whole stage).
   `compose.cpp` sets `eq.on` for ON and ON+CUTS and `eq.cuts` for ON+CUTS only.
2. The front half runs the cuts through a smoother of its own on `on && cuts`, and holds no filter
   state for them while settled without them. The de-esser reads the cut signal at ON+CUTS and the
   input at ON.
3. The back half is unchanged. One trim (+1.3 dB) serves both settings.

## Names
VOX EQ in the menu, the emulator, the bench op lines (`[bench] VOX EQ forced off`), the stress table and
help, `cubevox-render` (`--vox-eq off|on|cuts`, default cuts), the proof tools and the docs. The code
identifiers `Polish`, `kPolishEq`, `BenchOp::PolishEq` and the bench op id `polish_eq` stay.

## Supersedes
1. `2026-10-24-polish-design.md` menu row 7: `POLISH EQ`, default ON, ON/OFF becomes `VOX EQ`, default
   OFF, OFF/ON/ON+CUTS.
2. `2026-10-31-polish-eq-adam-voice-design.md` "Menu": the one ON/OFF row. "Scope": the filters always
   run under the stage's one switch.
3. `2026-10-12-layout-record-design.md` byte 3 and `2026-10-12-serial-protocol-design.md` `polish_eq`.

## Tests
`polish_test` (OFF bit-exact, ON no cut at 50 Hz or 10 kHz, ON+CUTS both cuts, the fade, the de-esser at
ON), `level_test` (VOX EQ at ON and ON+CUTS, alone and in the tail), `firmware_test` (menu row, three
settings in order, OFF default, record round trip, old record with POLISH EQ ON loads OFF, protocol
strings, compose, chain), and the Go tests (unchanged).
