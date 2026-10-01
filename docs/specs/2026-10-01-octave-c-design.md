# cubevox — Octave option C (asynchronous granular shifter, pedal octaver)

Date 2026-10-01. Owner: "do that for option C". A and B are pitch-synchronous
(PSOLA, formants preserved). C is the classic pedal octaver: fixed-window
crossfaded grains, no tracker, formants move with pitch.

## Decisions

1. Same mechanism as Unison's fixed-detune shifter (`readShifted` in `engine/unison.h`):
   sawtooth read phase over a window, N taps spaced 1/N of a cycle apart, Hann gains
   summing to 1, Catmull-Rom reads from the shared `VoiceRing`. Generalised to N = 2 or
   4 grains and a 20–80 ms window.
2. No tracker dependency: `prepare` ignores `PitchResult` except that the shared
   front end still runs it for A/B/Harmony. Works on breath and sibilants.
3. FORMANT does not apply (greyed in the face when engine C is selected).
4. Shares SEMITONES, MIX, `levelDb`, `glideMs`, `on` with A and B; `muteUnvoiced` is
   ignored by C (no voiced decision available without trusting the tracker).

## Engine

### `engine/octave.h` (params extended)

```
struct OctaveTuning {
  ... existing ...
  // Option C
  float grainWindowMs = 40.0f;   // 20..80
  int   grainCount = 2;          // 2 or 4
  float trimDbC = 0.0f;          // level rule trim, set by level_test pass
};
// OctaveParams::engine: 0 = A, 1 = B, 2 = C
```

### `engine/octave_c.h`

```
class OctaveVoiceC {
 public:
  void reset();
  bool prepare(const PitchResult& pr, const OctaveParams& p);  // true if semitones != 0
  float tick(const VoiceRing& ring, long writeCount);
};
```

Behaviour:

1. `ratio = 2^(semis/12)`, semis glided at `glideMs` like A/B.
2. Window `W = grainWindowMs · fs/1000` samples. Phase `φ` advances
   `(1 − ratio)/W` per sample (wrapped to [0,1)). Grain k (k = 0..N−1) reads at delay
   `D_k = D0 + W · (frac(φ + k/N) − 0.5)` with `D0 = W/2 + 4` so reads stay ≥ 4 samples
   behind the write head for ratio ≤ 2 and never exceed the ring; gain
   `g_k = 0.5(1 − cos(2π · frac(φ + k/N)))`, normalised so Σ g_k = N/2 → divide by N/2.
3. Output × `dbToLin(levelDb + trimDbC)`.
4. Latency: fixed `D0 ≈ W/2`, 20 ms at the default window (lower than A/B's 25–35 ms).
5. Float only, `std::array`, no heap/I/O, `-Werror` clean.

### `engine/pitch_fx.h`

Owns `OctaveVoiceC c_`; selects by `engine == 2`; resets the unselected engines on
switch as now.

## Emulator / render

OCTAVE `Engine` radio A | B | C. FORMANT disabled for A and C. Tuning node gains
"Grain window (C)" 20–80 ms, "Grains (C)" radio 2 | 4, and "Trim C" dB. Render:
`--oengine 2`; keys `octGrainWindowMs`, `octGrainCount`, `octTrimDbC`.

## Tests (`test/octave_test.cpp` extended)

1. C pitch: engine 2, +12 and −12 on the 220 Hz tone → 440 / 110 Hz ±15 cents
   (granular shifters carry window-rate sidebands; the tracker's median handles it).
2. C formants move: engine 2, −12 st on the ten-harmonic tone; spectral centroid of the
   OUTPUT expressed as a ratio to its own f0 is within 10 % of the input's
   centroid/f0 ratio (i.e. the envelope scaled with the pitch), whereas engine A at
   −12 keeps the absolute centroid within 10 % of the input's. This is the one
   measurable difference between the two methods.
3. C bypass: engine 2, semitones 0 → bit-exact passthrough.
4. C no tracker dependence: engine 2, −12 st on white noise → output RMS within
   ±3 dB of input RMS × dbToLin(levelDb + trimDbC) (A/B would mute or misfire on
   noise; C must not).
5. `level_test` gains a row "octave C −12 mix .5" and `trimDbC` is set so it lands in
   0..+0.5 dB.

## Done when

`ctest` passes; OCTAVE offers A | B | C; owner A/Bs the three on octave down.
