# cubevox

Live vocal effects box for Adam Keith. Tabletop, SM58 in, one knob per parameter.
Final target: STM32H7 (H7 VIT6 board). Effects are prototyped one at a time in a
Mac emulator that plays a loaded loop through the effect while the knob is tweaked.

## Rules
- `engine/*.h` is firmware code. Float only, `std::array` state, no heap / no I/O in
  `process()`, builds with `-Wall -Wextra -Werror`. The H7 includes these files unchanged.
- The emulator mirrors the panel exactly. Dev-only controls live under a collapsed
  `Tuning` header. Never invent panel controls.
- Gates never open a window: `cubevox-proto --layout` and `ctest` are the checks.
- Specs live in `docs/specs/`. Current: `2026-10-01-unison-emulator-design.md`.

## Build / run
```
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
open build/cubevox-proto.app
build/cubevox-render in.wav out.wav --depth 0.7
```

## Panel (planned)
Input Gain · Soft gate (menu) · Autotune Key+Correction · Octave Semitones+Mix ·
Unison Depth · Slapback Intensity · Distortion Drive · Gate Threshold ·
Spring Tension+Dwell · Output (menu)
