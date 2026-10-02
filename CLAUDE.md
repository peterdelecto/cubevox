# cubevox

Live vocal effects box for Adam Keith. Tabletop, SM58 in, one knob per parameter.
Final target: STM32H7 (H7 VIT6 board). Effects are prototyped one at a time in a
Mac emulator that plays a loaded loop through the effect while the knob is tweaked.

## Rules
- `engine/*.h` is firmware code. Float only, `std::array` state, no heap / no I/O in
  `process()`, builds with `-Wall -Wextra -Werror`. The H7 includes these files unchanged.
- The emulator mirrors the panel exactly. Dev-only controls live under a collapsed
  `Tuning` header. Never invent panel controls. Exception (owner 2026-10-01): every
  effect block carries an on/off checkbox in the prototype so stages can be A/B'd.
- Face layout (owner 2026-10-01): four columns, no scroll bar, window 1440x840 (the
  owner's laptop shows ~847 px of window). Each effect block has its own collapsed
  `Tuning` header directly below it; one Tuning header open per column at a time
  (accordion), which is the invariant the `--layout` probe checks. A footer strip holds the bottom-right "Pedal mode" (with BYPASS above it, dry input out,
  stage feedback still running)
  button (owner 2026-10-02): pedal mode draws each effect as a stompbox with only its
  panel knobs (encoders below knobs), name and on/off LED at the foot; the probe checks it fits.
- Level rule (owner 2026-10-01): every stage engaged at its default setting outputs
  near unity, 0 to +0.5 dB RMS over its input on program material. The trim that
  gets a stage there lives in its tuning (wetDb / trimDb); the panel knobs never
  carry a hidden level offset. `level_test` enforces it. Stages with a MIX knob
  (Harmony, Octave, Reverb) are trimmed so the WET path alone at MIX 100 % is unity
  (Harmony: one voice at High); the equal-power crossfade then stays near unity at
  every MIX. Trimming at MIX 50 % instead pushed the wet +8 dB and clipped at full mix
  (owner 2026-10-02).
- Version (owner 2026-10-02): the face shows `VERSION.txt` bottom left (started v0.01.00).
  Every change that rebuilds the app bumps it first with `tools/bump_version.sh`, in the
  same commit. Not named `VERSION`: the Mac's case-insensitive disk lets `<version>` find it.
- Gates never open a window: `cubevox-proto --layout` and `ctest` are the checks.
- Stage feedback simulator is prototype-only test signal. It is calibrated so a bypassed
  box does not feed back at default Amount; DRIVE causes it.
- Specs live in `docs/specs/`. Current: `2026-10-01-gate-design.md`; one spec per stage under `docs/specs/`, all still apply.

## Build / run
```
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
open build/cubevox-proto.app
build/cubevox-render in.wav out.wav --depth 0.7
```

## Panel (planned)
Input Gain · Input gate Threshold+Decay (same controls as Gate) · Autotune Key (shared encoder)+Response (NATURAL → MECHANICAL, no mix) · Harmony: off the face, forced off; engine code kept (owner 2026-10-02) ·
Octave Semitones+Mix ·
Unison Depth+Rate · Slapback Intensity (level + repeats)+Time (30..150 ms) · Distortion Drive+Tone · Gate Threshold+Decay ·
Reverb Intensity (mix + decay)+Dwell (engine SPRING / CHASM / PARKER SPRING in menu) · EQ (menu) · Output (menu)
