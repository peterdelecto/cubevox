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
5. Hears at ON. A 40 Hz subsonic high-pass and the de-esser run ahead of slot 1, and the 300 Hz dip,
   3.5 kHz presence and trim run after POLISH COMP. Nothing audible is cut at the ends. The subsonic
   high-pass (4th-order Butterworth) sits under any note Adam sings and takes 3.0 dB at 40 Hz, 0.7 dB at
   50 Hz and 0.01 dB at 97 Hz (`polish_test` "subsonic"). The de-esser now reads the sibilant band at full
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
   on Adam's three clips with the one trim at +1.3 dB, VOX EQ alone reads +0.27 dB at ON and +0.14 dB
   at ON+CUTS, both inside 0 to +0.5 dB. Through the whole tail (front half, POLISH COMP, back half)
   the three clips read -0.20, +0.47 and +0.95 dB at ON (mean +0.40) and -0.34, +0.27 and +0.69 dB at
   ON+CUTS (mean +0.21). `level_test` judges all of these rows: each clip inside -1.5 to +1.0 dB (the
   comp levels the clips, so its own three readings spread past 0 to +0.5) and each mean inside 0 to
   +0.5 dB. Without the subsonic high-pass the ON tail read -0.14, +0.65 and +1.88 dB (mean +0.80).
   Clip 3 carries rumble under 70 Hz, 11 dB under its level and mostly under 40 Hz, and 68 % of it
   falls in quiet blocks where POLISH COMP reduces by under 1 dB. The comp's makeup lifts that band by
   +5.0 dB against +1.3 dB for the rest of the clip. The energy counts in the RMS and carries no
   loudness. A wider detector band did not close the gap, and a makeup that spared the subsonic band
   closed about a third of it, so the filter takes the energy out.
10. Goes wrong, the bench. At OFF the stress run's VOX EQ bucket reads near zero, so its gate against
    `kPolishEq` passes without measuring the stage. The run puts VOX EQ at ON+CUTS with each
    layout (`set_layout` carries `settings.vox_eq` `"cuts"`) and puts the box's own setting back at the
    end (`2026-11-01-stress-bench-design.md`). The row `kPolishEq` in `core/costs.h` stays the
    ON+CUTS cost. The subsonic high-pass adds two biquads to the front half, about 0.2 % of a block by
    instruction count, inside the row's headroom (mean 1.7 % of 2 %, worst 2.4 % of 3 %); the next
    stress run on the box confirms it.

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
2. The front half runs the subsonic high-pass (`kPolishSubsonicHz`, 40 Hz, two biquads) on the input
   whenever the stage is on, then the cuts through a smoother of its own on `on && cuts`, and holds no
   filter state for them while settled without them. The de-esser reads the cut signal at ON+CUTS and
   the subsonic-filtered input at ON.
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
`polish_test` (OFF bit-exact, ON no cut at 97 Hz or 10 kHz, the subsonic high-pass, ON+CUTS both cuts,
the fade, the de-esser at ON), `level_test` (VOX EQ at ON and ON+CUTS, alone and in the tail, every row
judged), `firmware_test` (menu row, three
settings in order, OFF default, record round trip, old record with POLISH EQ ON loads OFF, protocol
strings, compose, chain), and the Go tests (unchanged).
