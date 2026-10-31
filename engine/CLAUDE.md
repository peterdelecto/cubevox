# engine

- `engine/*.h` is firmware code. Float only, `std::array` state, no heap / no I/O in
  `process()`, builds with `-Wall -Wextra -Werror`. The H7 includes these files unchanged.
- No libm inside a per-sample loop. Use `engine/fastmath.h` (each function pinned against libm
  in `fastmath_test`), or evaluate at the block edges and interpolate. libm runs per block or
  when a setting changes.
- Settled off is a plain copy: a stage switched off ends bit-exact on its input. Test it with
  `==`, never a tolerance.
- A voice resets on its enable edge only when its crossfade sits at exactly 0; a quick off and
  on keeps the running voice. `knob_step_test`, `octave_test` and `autotune_test` hold every
  knob step and toggle to steady-state curvature.
- `process()` takes any `n`. An engine with kBlock-sized scratch (Reverb) runs the call in
  kBlock chunks.
- Shared pieces live in `engine/dsp.h` (Biquad and its RBJ setters, OnePole, `catmullRom`,
  `wrapOnce`, `clampf`, `kTwoPi`) and `engine/smooth.h` (`kSmoothCoef`, the 20 ms one-pole).
- The box runs one variant per stage: Autotune grid PSOLA, the granular Octave, Spring and
  Chasm reverbs, Distortion 2x oversampled, POLISH COMP at FIRM. Nothing else stays compiled.
- `PitchFx` lives in DTCM: 121,080 B of the 130,816 B `.dtcm_bss` region (`cubevox.ld`); the 230 ms
  input ring is 44,160 B of it, sized for the Gang card's 200 ms lag, and the 800 Hz shadow ring beside
  it (the Gang card's epoch search reads it) is another 44,160 B. The build prints DTCM at 121,088 B,
  92.6 %, 9,728 B free.
  Check `arm-none-eabi-nm -S` on the firmware elf after any pitch change.
- After an engine change that moves cycles, re-bench `firmware/cubevox/src/core/costs.h` with
  both passes its method comment describes.
