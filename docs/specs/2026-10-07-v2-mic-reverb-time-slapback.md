# cubevox v2: live mic, reverb TIME, slapback 60..250 ms

Date 2026-10-07. Adam on v0.01.22: "I could give a lot more info if I were able to run a mic
thru the prototype." Reverb DWELL "not helpful beyond 50 %", a time option would serve
better. Slapback: "I don't think I'd be likely to use anything shorter than 70 ms."
Owner: ship as v2.n.n through the existing install line; full view and pedal mode both change.

## Decisions

1. Reverb knobs are INTENSITY (mix) and TIME (decay). `ReverbParams::time` drives every
   engine's decay through the existing curves, so TIME 50 % equals the old INTENSITY 50 %
   decay. DWELL (soft-clip input drive) stays as a tuning field at 0.20, off the face.
   Adam's starting point sets TIME 100 % so the v0.01 sound is unchanged until he turns it.
2. Slapback TIME runs 60..250 ms, default 100, linear taper, 260 ms line. The level row was
   a coin flip on TIME; see the slapback spec's level correction (`trimDb` -4.9 dB x INTENSITY).
3. The transport row gains MIC. On, the loop group's slots become the capture device name
   and a gain (-12..+24 dB). The device is duplex on whatever macOS has chosen for input and
   output (System Settings > Sound) and follows a change while running; no in-app picker.
   No mic or no permission: playback only, MIC greyed. Latency: 128-frame period plus the
   pitch block's 25..35 ms when Autotune or Octave is on.
4. Version 2.00.00 and up; `tools/bump_version.sh` keeps the two-digit fields.

## Files

`engine/reverb.h`, `engine/slapback.h`, `emulator/main.cpp`, `emulator/Info.plist.in`
(NSMicrophoneUsageDescription), `test/level_test.cpp`, `test/slapback_test.cpp`.

## Done when

`ctest` and `--layout` pass; Adam installs with the curl line, flips MIC, sings.
