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

## Box code (v2.01.10, audit fix pass 2026-11-02)

Option C is the only Octave engine, `OctaveVoice` in `engine/octave.h`. Sections above are the
2026-10-01 design; where they differ, this section is the box.

1. Two taps only. The 4-grain option and `grainCount` are gone. The taps share one 32-bit
   sawtooth counter and one Hann rotor, with delays and ring positions in Q18 samples. The
   window is 20 to 80 ms (`grainWindowMs`, default 40), rounded to an even number of samples.
2. One fade. `PitchFx`'s `octaveFade_` is the only fade on the toggle; the voice has no gain fade
   of its own. The mix turns `MIX × octaveFade_` through a quarter turn, so dry and wet stay
   equal-power at every fade. SEMITONES 0 switches the voice off through the same fade, so
   SEMITONES through 0 is an equal-power crossfade to the dry voice and never cuts it.
3. The voice resets on its enable edge only once `octaveFade_` sits at 0 (`octave_test` through
   0). A NaN in the voice latches until a reset; the chain's NaN guard (`discardBlock`) resets
   every engine.
4. Autotune's correction reaches the voice through its own 2 ms one-pole (`kOctaveCorrCoef`), so
   the voice follows a vibrato on the tuned note instead of lagging it by the 15 ms SEMITONES
   glide.
5. The wet passes a 5 Hz DC blocker before the mix; under 1e-30 its state rests at 0.
6. Level: `OctaveTuning::trimDb` 1.1 dB, the wet alone at MIX 100 % on Adam's clips. Clip rows
   (mean) read +0.22, +0.21, +0.23 and +0.23 dB at −12, −5, +7 and +12 st, each clip +0.13 to
   +0.29 dB; MIX 50 % reads −0.16 to +0.09 dB. The synthetic-program row prints as a NOTE.
7. MIX starts at 10 % (`kOctaveDefaultMix`); the registry, the render tool and the tests read it
   there.
8. CPU: the fast loop is 65 M7 instructions a sample, against 238 for the old per-sample
   `OctaveVoice::tick`. Host bench on Adam's clips, ns a sample (clang / g++ 15): Octave +12 st
   MIX 100 % 17.6 / 11.8 before, 4.8 / 4.95 after; the default layout's pitch slot (Autotune and
   Octave) 46.1 / 41.4 before, 30.3 / 31.9 after. `kOctave` is {2, 2, false}, an estimate from
   that bench until a stress run of the new engine.
9. Small SEMITONES pulse (settled 2026-11-02, the lead's call for the owner). The two taps keep
   their 20 ms spacing, so at ±1 or ±2 st the comb they form sweeps through a held note once every
   0.33 to 0.71 s; a steady 220 Hz tone swings up to 6 dB, a 100 Hz tone about 1 dB. Kept: it is
   how an asynchronous two-tap shifter sounds, the card is for octaves and fifths (+12 the
   quietest setting), and Unison covers small detunes. A cure would be a pitch-synchronous voice.
