# cubevox — Octave option B (epoch-aligned PSOLA with formant control)

Date 2026-10-01. Owner: "explore an alternative octave engine (Octave option A and
this one Octave Option B). It needs to be more natural-sounding and possibly have a
formant control." Option A is `engine/octave.h` (grid PSOLA, formants preserved,
no formant knob). Both run on the shared front end in `engine/pitch_fx.h`.

## Decisions

1. Option B is pitch-synchronous overlap-add like A, with two changes aimed at
   "natural": grains are centred on the glottal peak of each period (epoch
   alignment) rather than on an arbitrary grid, and each grain is read at its own
   rate (the formant ratio) so vowel size is independent of pitch.
2. FORMANT is a signed semitone control, −12..+12, 0 = vowels preserved. It is a
   prototype control in the OCTAVE block; whether it becomes a panel knob is the
   owner's call after listening.
3. Engine select A/B is a prototype radio in the OCTAVE block, in the params so the
   render CLI can A/B too.
4. SEMITONES and MIX stay shared between A and B.

## Engine

### `engine/octave.h` (params extended)

```
struct OctaveTuning {
  float levelDb = 0.0f;
  float glideMs = 15.0f;
  bool muteUnvoiced = false;
  // Option B
  float grainPeriods = 2.0f;     // grain length in sung periods, 1.5..3
  float epochSearch = 0.25f;     // search window for the peak, fraction of a period
  float epochLpHz = 1000.0f;     // low-pass used to find the peak
};

struct OctaveParams {
  bool on = true;
  int engine = 0;                // 0 = A (grid PSOLA), 1 = B (epoch PSOLA + formant)
  int semitones = 0;             // panel knob, -12..12, 0 = off
  float formant = 0.0f;          // option B, -12..12 semitones, 0 = preserved
  float mix = 0.5f;              // panel knob
  OctaveTuning tuning;
};
```

### `engine/octave_b.h`

```
class OctaveVoiceB {
 public:
  void reset();
  bool prepare(const PitchResult& pr, const OctaveParams& p);  // true if active
  float tick(const VoiceRing& ring, long writeCount, float period);
};
```

Behaviour:

1. Shares `VoiceRing`, `kGrainDelay` and the launch clock with `PsolaVoice`: output
   grains launch every `period / ratio` samples, `ratio = 2^(semis/12)` glided.
2. Epoch alignment: the candidate centre is the grid mark nearest
   `writeCount − kGrainDelay` as in A. Within `± epochSearch · period` of it the
   voice picks the sample with the largest value of the ring low-passed at
   `epochLpHz` (a one-pole run on the ring input, state kept per voice; keep a
   parallel `std::array` of the low-passed signal the size of the ring, written by
   `PitchFx` once per sample and passed to `tick`, so the search is a read not a
   filter run). Successive centres are kept at least `0.75 · period` apart so a
   double peak cannot launch two grains on the same cycle.
3. Formant: grain playback rate `f = 2^(formant/12)`. A grain of nominal length
   `grainPeriods · period` input samples is read at `f` samples per output sample
   with Catmull-Rom, so it lasts `grainPeriods · period / f` output samples and its
   Hann window is over that duration. The read must stay at least 3 samples behind
   the write head. A grain spans `len` input samples centred on its epoch, so it
   starts `len/2` behind the epoch and reaches `len · (1 − 1/f)` toward the head,
   which is ≤ `len/2` for `f ≤ 2`; no start pull-back is needed. `static_assert`
   the head bound (f ≤ 2, epoch offset) and the ring bound at f = 0.5,
   grainPeriods = 3, epochSearch ≤ 0.3.
4. Gain normalisation `(f · 2 / grainPeriods) / ratio`. Grain amplitude in the
   overlap-add scales with grain duration `grainPeriods · period / f`, so this
   factor holds loudness constant across FORMANT and grain length (measured
   without it: formant +12 halves the level, grainPeriods 3 is 1.5× louder).
5. Same fresh-start, glide, level and muteUnvoiced rules as A.

### `engine/pitch_fx.h`

Owns `OctaveVoice a_` and `OctaveVoiceB b_`, plus the low-passed shadow ring.
`prepare` and `tick` only the selected engine; the unselected one is `reset()` on
switch so it starts clean. The octave stage's active/mix smoothers are unchanged.

## Emulator

OCTAVE block: `Engine` radio `A | B` after the checkbox, `SEMITONES`, `FORMANT`
int slider −12..+12 `%+d st` (greyed when engine A), `MIX`. Tuning node gains the
three option-B fields.

## Render CLI

`--oengine <0|1>`, `--formant <-12..12>`. Tuning keys `octGrainPeriods`,
`octEpochSearch`, `octEpochLpHz`.

## Tests (`test/octave_test.cpp` extended)

1. B pitch: engine 1, +12 and −12 on a 220 Hz tone → 440 / 110 Hz ±10 cents.
2. B formant leaves pitch alone: engine 1, semitones +7, formant +12 → f0 329.63 Hz
   ±10 cents.
3. B formant moves the envelope: engine 1, semitones 0, formant +12 vs 0 on the
   ten-harmonic tone; spectral centroid (Goertzel on harmonics 1–10 of the OUTPUT
   pitch) is ≥ 1.2× higher with formant +12 (the exact ×2 stretch of this 1/k tone
   scores 1.24 once its odd harmonics fall between the lines). Formant −12 is
   ≤ 0.8×.
3a. B formant gain is exact: the matched harmonic keeps its amplitude within
   0.2 dB (output harmonic 2 vs input harmonic 1 at formant +12; output harmonic 1
   vs input harmonic 2 at −12). Total RMS is not a valid check here: shifting the
   envelope of a 1/k tone legitimately moves its RMS (−6.3 dB at −12, +2.2 dB at
   +12), and the engine reproduces those figures exactly.
4. B bypass: engine 1, semitones 0 → bit-exact passthrough.
5. Existing checks 1–6 unchanged (engine 0).

## Done when

`ctest` passes; OCTAVE block shows A/B and FORMANT; owner listens to A vs B on
octave down and up with a vocal loop.
