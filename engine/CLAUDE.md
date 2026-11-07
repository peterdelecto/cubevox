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
     `run`), `catmullRom`, `catmullRomWeights`, `wrapOnce`, `readRing`.
  3. `engine/fastmath.h`: `exp2Fast`, `log2Fast`, `dbFast`, `dbToGainFast` (the per-sample dB law),
     `DbGain`, `sinTurns`, `sinCosFast`, `tanhFast`, `HannRotor`, and the constants `kLog2E`, `kDbPerLog2`,
     `kLog2PerDb`, `kTurn32`.
  4. `engine/smooth.h`: `kSmoothSec`, `kSnap`, `kSmoothCoef`, `kSmoothKeepBlock`, the literals `kSmoothBlockCoef`, `kKeep50msSample`, `kKeep50msLog2` and
     `kSmooth5msCoef`, `BlockCoef` (the 20 ms coefficient for the last chunk length, cached), `clamp01`, `keep` (what a
     one-pole keeps over n samples), `blockCoef`, `coef`, `step`, `advance` (n steps of one smoother or of two
     that share a coefficient), `advanceBy` (n steps in closed form) and `advanceBlock` (kBlock steps of the
     20 ms smoother).
  5. `engine/scale.h`: the key tables, `kMajor`, `midiOf`, `scaleDeviation`, `kHoldSemis`, `heldNote`.
  6. `engine/reverb_parts.h`, for the two reverbs: `softClip`, `softClipHeadroom`, `kReverbClipHeadroom`, the
     allpass step, `Allpass`, `Delay`, `KnobCurve`, `kFeedbackMax`, `kMaxWobbleHz`, the wobble LFO (`lfo`, on `sinTurns`), its phase step and
     error bound.
- The board links its own memset, memcpy and memmove (`firmware/cubevox/src/board/fastmem.cpp`, checked on the
  host by `fastmem_test`), which move 16 bytes a pass, about 0.3 to 0.5 cycles a byte by instruction count.
  newlib-nano's versions are byte loops at about 5 cycles a byte, so a `fill(0.0f)`, `std::copy` or struct
  copy that GCC turns into a call is fast. Keep the board's fastmem.cpp compiled with
  `no-tree-loop-distribute-patterns`, or its loops become calls to themselves. A clear the size of the pitch
  ring (51 kB, about 10k to 26k cycles, 2 to 4 % of a 640k-cycle block) is still spread: `InputRing::rest()`
  clears 512 floats a block while a card's slot is stopped (`AutotuneFx`, `OctaveFx`, `FormantFx`, `Crowd`,
  `Growl`), so the `reset()` at the switch-on finds the ring clear. The spread clears and the tracker's
  resets all use `std::fill_n` or `.fill(0.0f)`. `pitch_spikes_test` holds the spread.
- The onset estimate's 48 kHz refinement walks downhill from the coarse period and sums at most
  `kOnsetMaxLags` lags; summing all 13 put the estimate's block above a voiced hop's. The estimate skips the
  block that closes a hop, which runs the analysis; run there it put a block 0.4 us over a voiced hop's on clip 3.
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
- Each pitch card runs its own engine on the block the slot ahead hands it, with a ring of that input
  (`engine/input_ring.h`): `AutotuneFx` (ring 2,176 floats, the tracker, the corrected voice), `OctaveFx` (3,920
  floats), `FormantFx` (3,852 floats and a tracker of its own, for Formant and Apraxia). `Crowd` keeps a 51,024 B
  ring of Autotune's input, written by the Autotune slot, and `Growl` a 15,408 B ring. `AutotuneFx` and `OctaveFx`
  sit in DTCM (the build prints the figure, 130,816 B `.dtcm_bss` region in `cubevox.ld`),
  `FormantFx`, `Crowd` and `Growl` in D1 inside `gChain`. `Distortion` sits in DTCM too
  (2,888 B: its filters, the rail histories and the block scratch), and so does `Unison` (10,216 B, mostly
  its 50 ms line).
  Check `arm-none-eabi-nm -S` on the firmware elf after any pitch change.
- Autotune's tracker serves the Formant engine directly behind it (only Empty slots between) and any Crowd or
  Growl card behind it. Anywhere else those cards run a tracker of their own on their own ring. The chain hands
  the readers an `UpstreamPitch` (Autotune's tracker result, the shift the slots since have put on the pitch, the
  delay they have put on the audio, the Autotune engine for the Formant engine) and a `CrowdPitch`. Mob is the one
  exemption from the chain-order rule: it reads Autotune's input, written into its ring by the Autotune slot.
- Grain cards next to each other share one grain pass (`2026-11-05-grain-group-design.md`). The Formant engine
  directly behind a running Autotune cuts its grains from the audio that entered Autotune, so the
  formant-shifted voice waits for Autotune's delay once and the chain-order rule still holds. The Autotune slot
  hands that input to the card (`FormantFx::feed`), which writes it into its own ring, so Autotune's ring and
  DTCM stay as they were. The audio the card receives is `tuned` times Autotune's corrected voice plus `dry`
  times Autotune's input (`AutotuneFx::groupFeed()`, with the slot's engine gain from `UpstreamPitch`), and the
  card renders those shares from the input they came from. `FormantFx::voice_` cuts the input at pitch ratio 1 and `tuned_` at Autotune's
  ratio, both at the shift's grain rate and Autotune's delay, mixed by Autotune's linear fade, and each runs only
  while its share is up. All three voices lay their grains on one grid (`GridMarks`): Autotune's corrected voice
  takes the ratio 1 voice's marks when it starts (the chain passes them to `AutotuneFx::process`), and each Formant
  voice that starts takes the marks of the Autotune voice. Autotune's toggle then moves the Formant voice from one
  share to the other at one delay, with no jump in the read position. Settled off, `voice_` alone runs and its
  output equals the card on its own ring and its own tracker, bit for bit. The base is the audio as it arrives
  with Autotune's input share read at the voice's delay. A call longer than kBlock, or an `UpstreamPitch` without
  the engine or without a `feed` of that block, runs the card on its own ring and tracker.
- The Growl card (Deepthroat) grows what reaches its slot. It keeps a ring of its own input, `growl_detail::kRingLen`
  samples (15,408 B, a plain global in D1 like Crowd, so DTCM is unchanged), and cuts its grains from that on a
  `FormantPsolaVoiceT<kRingLen>`. The period it cuts on is the tracker's period divided by the shift in
  `UpstreamPitch` (Autotune's correction, or the Octave voice's interval when Octave holds half the output's
  power), and its read lag is the tracker's alignment less the delay in `UpstreamPitch`. A stopped Growl slot
  calls `Growl::rest()` each block (`runSlot`), which clears 2 KB of the ring (`InputRing::rest()`), so the reset at the switch-on finds
  it clear. `growl_test` measures both values against `OctaveFx` and `FormantFx`.
- The tracker reads its history from the caller's ring, so the ring must hold the whole call before
  `PitchTracker::push` runs (each engine writes its ring first). The tests use `RingedTracker`
  (`test/ringed_tracker.h`), which keeps a ring of its own, and `PitchSeries` (`test/pitch_series.h`), which
  runs Autotune, Formant and Octave in series as three adjacent slots. The three Adam clips in `samples/` are
  the cleaned set of 2026-11-05 (4th-order Butterworth high-pass at 40 Hz, zero phase, forward and backward);
  the originals live in `samples/original/` and no test reads them.
- After an engine change that moves cycles, re-bench `firmware/cubevox/src/core/costs.h` with
  both passes its method comment describes.
