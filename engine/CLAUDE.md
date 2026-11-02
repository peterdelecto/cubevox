# engine

- `engine/*.h` is firmware code. Float only, `std::array` state, no heap / no I/O in
  `process()`, builds with `-Wall -Wextra -Werror`. The H7 includes these files unchanged.
- No libm inside a per-sample loop. Use `engine/fastmath.h` (each function pinned against libm
  in `fastmath_test`), or evaluate at the block edges and interpolate. libm runs per block or
  when a setting changes.
- Settled off is a plain copy: a stage switched off ends bit-exact on its input. Test it with
  `==`, never a tolerance.
- A voice resets on its enable edge only when its crossfade sits at exactly 0; a quick off and
  on keeps the running voice. `knob_step_test`, `octave_test`, `autotune_test` and `formant_test`
  hold every knob step and toggle to steady-state curvature.
- `process()` takes any `n`. An engine with kBlock-sized scratch (Reverb) runs the call in
  kBlock chunks.
- Shared pieces live in `engine/common.h` (`kSampleRate`, `kBlock`, `kSmpPerMs`), `engine/dsp.h` (Biquad and its RBJ
  setters, OnePole, `catmullRom`, `wrapOnce`, `readRing` and `readRingClamped` for a delay line that may leave
  [2, Len - 3], `clampf`, `kTwoPi`) and `engine/smooth.h` (`kSmoothCoef`, the 20 ms one-pole, `blockCoef` for a
  one-pole stepped once per block, `advance` for n steps of one smoother or of two that share a coefficient, and
  `advanceBlock` for kBlock steps in closed form).
  The two reverbs share `engine/reverb_parts.h` (the allpass step, Allpass, Delay, the wobble LFO's sine table,
  phase step and error bound, and the Dwell drive terms).
- Spring runs its loops at 24 kHz between a 15-tap half-band decimator and interpolator. Its
  delay lengths subtract the filters' delay (`kFilterDelay`), and the tick stays inlined in
  `Spring::run`, so check the M7 listing for a call to it after any change there.
- The box runs one variant per stage: Autotune grid PSOLA, the granular Octave, Spring and
  Chasm reverbs, Distortion 2x oversampled, POLISH COMP at FIRM. Nothing else stays compiled.
- `PitchFx` lives in DTCM: about 61,400 B of the 130,816 B `.dtcm_bss` region (`cubevox.ld`), the
  Formant voice included; the 265.75 ms input ring is 51,024 B of it, sized in `psola.h` for the
  Crowd card's 225 ms lag ceiling (a raised copy 75 ms behind a tuned copy at 150 ms), 12 ms of
  scatter and the longest grain. `Distortion` sits there too (2,624 B: its filters, the rail histories
  and the block scratch). The build prints DTCM at 64,064 B, 49.0 %, 66,752 B free.
  Check `arm-none-eabi-nm -S` on the firmware elf after any pitch change.
- The tracker reads its history from the caller's ring, so the ring must hold the whole call before
  `PitchTracker::push` runs (PitchFx writes it first). The tests use `RingedTracker`
  (`test/ringed_tracker.h`), which keeps a ring of its own.
- After an engine change that moves cycles, re-bench `firmware/cubevox/src/core/costs.h` with
  both passes its method comment describes.
