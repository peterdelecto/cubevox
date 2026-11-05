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
- Shared pieces live in the headers below. A stage uses them and keeps no private copy of a helper or a
  constant, so a fix lands once. Grep here before writing a clamp, a taper, a dB law, a corner or a smoother.
  1. `engine/common.h`: `kSampleRate`, `kBlock`, `kSmpPerMs`, `CV_INLINE`, `CV_RESTRICT`.
  2. `engine/dsp.h`: `kPi`, `kTwoPi`, `kHalfPi`, `kFilterMinHz`, `kFilterMaxHz`, `kButterworthQ`, `clampf`,
     `logTaper`, `dbToGain` (libm, exact, for a value computed once a block or on a setting change),
     `onePoleCoef` and `onePoleCoefAt`, `OnePole`, `kWetDcBlockHz`, `dcBlockCoef`, `DcBlocker`, `RbjCorner`
     with `rbjCorner`, `rbjAlpha` and `rbjAmp`, `kPeakMinQ`, `kShelfMaxSlope`, `kShelfMinSlope`, `Biquad`
     (the dB setters, the `...Amp` setters that take a kept corner and a linear amplitude, `setCoefsFrom`,
     `run`), `catmullRom`, `catmullRomWeights`, `wrapOnce`, `readRing`, `readRingClamped` for a delay
     that may leave [2, Len - 3], and `zeroFloats`.
  3. `engine/fastmath.h`: `exp2Fast`, `log2Fast`, `dbFast`, `dbToGainFast` (the per-sample dB law),
     `DbGain`, `sinTurns`, `sinCosFast`, `tanhFast`, `HannRotor`, and the constants `kLog2E`, `kDbPerLog2`,
     `kLog2PerDb`, `kTurn32`.
  4. `engine/smooth.h`: `kSmoothSec`, `kSnap`, `kSmoothCoef`, `kSmoothKeepBlock`, `clamp01`, `keep` (what a
     one-pole keeps over n samples), `blockCoef`, `coef`, `step`, `advance` (n steps of one smoother or of two
     that share a coefficient), `advanceBy` (n steps in closed form) and `advanceBlock` (kBlock steps of the
     20 ms smoother).
  5. `engine/scale.h`: the key tables, `kMajor`, `midiOf`, `scaleDeviation`, `kHoldSemis`, `heldNote`.
  6. `engine/reverb_parts.h`, for the two reverbs: `softClip`, `softClipHeadroom`, `kReverbClipHeadroom`, the
     allpass step, `Allpass`, `Delay`, `KnobCurve`, `kFeedbackMax`, the wobble LFO's sine table, phase step and
     error bound, and the Dwell drive terms.
- The board's memset and memcpy run a byte at a time (newlib-nano), about 5 cycles a byte, and
  `std::array::fill(0.0f)` compiles to memset. A clear of more than a few hundred bytes in a block that
  can land on a switch-on uses `zeroFloats` (a word a store), and a clear the size of the pitch ring
  (51 kB, 250k cycles by memset) is also spread: `PitchFx::rest()` clears a slice a block while the stage
  is stopped, so `reset()` at the switch-on finds it clear. `pitch_spikes_test` holds both.
- The onset estimate's 48 kHz refinement walks downhill from the coarse period and sums at most
  `kOnsetMaxLags` lags; summing all 13 put the estimate's block above a voiced hop's.
- `clampf`, `clamp01`, `KnobCurve::at` and `exp2Fast` return the lower bound for a NaN, so a NaN stops at the
  first boundary clamp. The float-to-int casts in `exp2Fast`, `sinCosFast`, `readRing` and `Delay::read` see a
  finite value for any input.
- The box sets flush-to-zero in `cubevox.ino`, so the engine adds no flush of its own for filter tails.
- The slot crossfade (`mixSlotFade` in `chain_run.cpp`) is equal-gain, a linear pair that sums to 1, because
  nearly every card's output carries its input and an equal-power pair would swell it by 3 dB. The BYPASS mix
  stays equal-power.
- Spring runs its loops at 24 kHz between a 15-tap half-band decimator and interpolator. Its
  delay lengths subtract the filters' delay (`kFilterDelay`), and the tick stays inlined in
  `Spring::run`, so check the M7 listing for a call to it after any change there.
- The box runs one variant per stage: Autotune grid PSOLA, the granular Octave, Spring and
  Chasm reverbs, Distortion 2x oversampled, POLISH COMP at FIRM. Nothing else stays compiled.
- `PitchFx` lives in DTCM: about 61,400 B of the 130,816 B `.dtcm_bss` region (`cubevox.ld`), the
  Formant voice included; the 265.75 ms input ring is 51,024 B of it, sized in `psola.h` for the
  Crowd card's 225 ms lag ceiling (a raised copy 75 ms behind a tuned copy at 150 ms), 12 ms of
  scatter and the longest grain. `Distortion` sits there too (2,888 B: its filters, the rail histories
  and the block scratch), and so does `Unison` (10,204 B, mostly its 50 ms line). `RingMod` sits there too
  (13,584 B: a `PitchTracker` of 8,472 B, its 1,260-float ring of 5,040 B, and the card's state). The build
  prints DTCM at 88,224 B, 67.4 %, 42,592 B free.
  Check `arm-none-eabi-nm -S` on the firmware elf after any pitch change.
- The Growl card (Deepthroat) grows what reaches its slot. It keeps a ring of its own input, `growl_detail::kRingLen`
  samples (15,408 B, a plain global in D1 like Crowd, so DTCM is unchanged), and cuts its grains from that on a
  `FormantPsolaVoiceT<kRingLen>`. The period it cuts on is the tracker's period divided by
  `PitchFx::outputShiftSemis()` (Autotune's correction, or the Octave voice's interval when Octave holds half the
  output's power), and its read lag is the tracker's alignment less `PitchFx::outputLatency()`. A stopped Growl slot
  calls `Growl::rest()` each block (`runSlot`), which clears 1 KB of the ring, so the reset at the switch-on finds it
  clear. `growl_test` measures both stage accessors against the stage.
- The tracker reads its history from the caller's ring, so the ring must hold the whole call before
  `PitchTracker::push` runs (PitchFx writes it first). The tests use `RingedTracker`
  (`test/ringed_tracker.h`), which keeps a ring of its own.
- Ringer tracks the pitch of its own input, the signal the slot ahead hands it, so it takes no part in the
  shared pitch stage and sits in no registry group. It owns a `PitchTracker` (coarse search only) fed from a
  ring of `PitchTracker::kRingNeed` floats, the most a push reads back with calls of up to `kBlock` samples.
  It clears the ring as `PitchFx` does: `RingMod::rest()` zeroes 512 floats a block while the slot is
  stopped, and `reset()` clears the rest with `zeroFloats`. `pitch_spikes_test` holds both.
- After an engine change that moves cycles, re-bench `firmware/cubevox/src/core/costs.h` with
  both passes its method comment describes.
