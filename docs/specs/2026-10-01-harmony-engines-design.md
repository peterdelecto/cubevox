# cubevox — Harmony shifter engines A | B | C, per-voice formant, chromatic mode

Date 2026-10-01. Owner: "both. /go". Harmony keeps its interval logic (key, scale
degree, voice tables) and gains a selectable shifter per voice, reusing the three
Octave engines, plus a chromatic (fixed-interval, no key) mode like pedal harmonizers.

## Decisions

1. `HarmonyParams::engine`: 0 = A (grid PSOLA, current), 1 = B (epoch PSOLA + formant),
   2 = C (asynchronous granular). Both voices use the same engine.
2. Per-voice FORMANT on B: `HarmonySlot::formant` (−12..+12 st, default 0), shown as a
   slider in each active Menu row when engine B is selected (greyed otherwise). Defaults
   suggested by TC-Helicon practice: none baked; owner sets by ear.
3. Chromatic mode: `HarmonyParams::chromatic` (bool). When true the interval per voice is
   a fixed semitone count from `HarmonyTuning::chromaticSemis[voice]` (defaults Lower
   −7, Low −4, High +4, Higher +7) and the key is ignored. The tracker still runs (A/B
   need the period; C does not use it).
4. Voice levels, glide, muteUnvoiced, mix, trim: unchanged. `muteUnvoiced` is ignored
   on C.
5. Each engine keeps its own level trim per the level rule: `HarmonyTuning::trimDb`
   becomes `trimDb[3]` indexed by engine; `level_test` gains rows for B and C.

## Engine

### `engine/harmony.h`

```
struct HarmonySlot {
  HarmonyVoice voice = HarmonyVoice::High;
  int level = 0;
  float formant = 0.0f;          // engine B only, semitones
};

struct HarmonyTuning {
  ... existing ...
  int8_t chromaticSemis[5] = {-7, -4, 0, 4, 7};   // Lower, Low, Fixed(unused), High, Higher
  float trimDb[3] = {8.4f, 0.0f, 0.0f};            // per engine; B and C set by level pass
  // option B/C fields mirror OctaveTuning: grainPeriods, epochSearch, epochLpHz,
  // grainWindowMs, grainCount — share them by reading from a HarmonyShiftTuning struct
  // with the same defaults as OctaveTuning's.
};

struct HarmonyParams {
  bool on = true;
  int engine = 0;                // 0 A, 1 B, 2 C
  bool chromatic = false;
  int key = 0;
  float mix = 0.5f;
  std::array<HarmonySlot, 2> slots{};
  HarmonyTuning tuning;
};
```

`HarmonyVoices` owns, per slot, one `PsolaVoice` (A), one `OctaveVoiceB`-style epoch
voice and one `OctaveVoiceC`-style granular voice. Factor the B and C voice classes so
they take (semis target, formant, gain) from the caller rather than from
`OctaveParams`: introduce `engine/shifters.h` with `EpochShifter` and `GrainShifter`
(the bodies move out of `octave_b.h` / `octave_c.h`, which become thin wrappers so
Octave B/C behaviour is unchanged and their tests still pass). `prepare` computes the
interval as now (diatonic) or from `chromaticSemis` (chromatic), then sets each
slot's shifter target; `tick` sums the selected engine's voices. Unselected engines
are reset on switch.

### `engine/pitch_fx.h`

Passes the low-passed shadow ring to Harmony's B voices as it does for Octave B.

## Emulator / render

HARMONY block: `Engine` radio A | B | C after the checkbox; `Chromatic` checkbox next
to KEY (KEY greyed when chromatic). Menu rows gain a `Formant` slider (−12..+12 st,
"%+d st") per row, enabled only when engine B and the row is active. Tuning: a
"Chromatic intervals" sub-node with the four semitone inputs, a "Shifter B/C"
sub-node (grain length, epoch search, epoch LP, grain window, grains), and Trim A/B/C.
--layout must pass (column 1; split nodes if needed). Render: `--hengine 0|1|2`,
`--chromatic`, `--voice name=level[:formant]`; keys `harChromLower/Low/High/Higher`,
`harTrimDbA/B/C`, `harGrainPeriods`, `harEpochSearch`, `harEpochLpHz`,
`harGrainWindowMs`, `harGrainCount`.

## Tests (`test/harmony_test.cpp` extended)

1. B and C pitch: engine 1 and 2, key C, High on A3 (220 Hz) → 261.63 Hz ±10 cents
   (B) / ±15 cents (C).
2. B formant: engine 1, High on A3, formant +12 on that slot → f0 still 261.63 ±10
   cents and centroid/f0 ≥ 1.2× the formant-0 case (band-summed harmonics as in the
   octave tests).
3. Chromatic: `chromatic = true`, High on A3 in key C → +4 st = 277.18 Hz ±10 cents
   (diatonic would give +3); Lower → −7 st.
4. C on noise: engine 2, High level 3, mix 1 on white noise → output RMS within ±3 dB
   of input × level × trim.
5. Existing checks unchanged on engine 0. `level_test` rows "harmony B" and "harmony C"
   at High level 2, mix 0.5, trims set to land in 0..+0.5 dB.
6. Octave tests unchanged (refactor must not change A/B/C behaviour).

## Done when

`ctest` passes; HARMONY offers A | B | C, per-voice FORMANT on B, Chromatic; owner
A/Bs the three on a third and a fifth.
