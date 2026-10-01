# cubevox — distortion stage (Boss BD-2 Blues Driver model)

Date 2026-10-01. Adam uses a Boss Blues Driver. Panel knob is DRIVE only (owner:
"its not intended to push an amp, so no level needed"). Stage is always on with a
menu bypass; in the prototype the block checkbox is that bypass. Engine contract,
emulator skeleton, threading unchanged.

References: analogisnotdead.com/article25 (topology) and an owner-supplied
breadboard analysis of the BD-2 / Cobalt circuit (numbers below come from it).

## BD-2 signal path (owner-supplied analysis)

1. Input buffer, 1 MΩ input impedance. Clean at low GAIN.
2. Gain stage 1: discrete op-amp (Q5 BJT makes most of the gain and sets the
   overload). Rail-to-rail swing, well-controlled overload, not FET-style distortion.
   Max gain a little over 40 dB (100×), peaking between 2 and 3 kHz. Bass rolls off
   from 700 Hz (R5/C4); C5 rolls off the top above the peak.
3. Fender tone stack, Treble 0 / Mid 6 / Bass 10. Mostly puts back the bass lost in
   stage 1 and loses a lot of level doing it. Because of that loss, stage 1 saturates
   long before the clipping diodes conduct. **The diodes do nothing audible**; the
   author scoped it with them in, out, and jumpered. They are omitted from the model.
4. Gain stage 2: same discrete op-amp, max gain just under 40 dB, flat 100 Hz to
   about 6 kHz. Both GAIN gangs turn together (audio taper, dual A250K).
5. Fixed treble cut: −6 dB above 1 kHz.
6. TONE: acts above 1 kHz, flat at noon. The panel TONE knob maps to the treble
   shelf gain, noon 0 dB, fully down `toneMinDb`, fully up `toneMaxDb`.
7. LEVEL, then a gyrator (32 H) resonating with C17 for a +6 dB bump at 120 Hz.
8. Max end-to-end gain 52 dB. Distortion fades out abruptly as a note decays, which
   is the signature of rail saturation rather than a soft knee.

Notes from the same author's Boneyard / BD-2w mod write-up (part 4):

1. Diodes confirmed inaudible a second time ("lightly clip the very lowest freq
   signals when GAIN is above noon", then stage 2 clips hard anyway).
2. BD-2w S/C switch = TIGHT (extra bass cut at the stage-2 input, about 2 dB more
   with 3.3 kΩ than 10 kΩ) + FAT (more capacitance in the TONE network, more treble
   cut). In this model TIGHT is `s2HpHz` raised, FAT is TONE turned down. Measured
   on the -2w (part 2): C position is 4.5 dB weaker at 50 Hz and 1 dB stronger at
   500 Hz at the stage-2 input; overall S vs C differs little, "a little louder in
   C". Owner 2026-10-01: Adam's BD-2 is stock, not modded, not a Waza. The notes
   above are reference only; the model is the stock pedal.
3. In clean mode the stock response is roughly flat below 500 Hz, so the stage-1
   shelf cut and the tone-stack bass boost should net near 0 dB there at low DRIVE.
4. The bass boost sits after the clippers. Once stage 2 limits everything to the
   rails, the post boost makes everything below 200 Hz louder than the rest. This is
   why `bassPeak*` is in the post chain and why its level matters at high DRIVE.

## Decisions

1. Behavioural model, one cheap block per stage above, every constant a tuning item.
2. DRIVE is the dual audio-taper GAIN pot: `g1 = gain1Max^d`, `g2 = gain2Max^d`
   (1× at DRIVE 0, full at DRIVE 1). DRIVE 0 is sample-exact passthrough so the
   always-on stage is transparent at zero.
3. The only nonlinearity is rail saturation, applied after stage 1 and after stage 2,
   as a clamp with a short soft edge. A small tunable rail asymmetry stands in for
   imperfect biasing (default 0.05).
4. No LEVEL knob. A static makeup law keeps output level within a few dB of input
   across DRIVE for a −12 dBFS vocal; `trimDb` adjusts it.
5. The two saturators run 2× oversampled (small half-band FIR) to keep aliasing off a
   vocal. Tuning switch to turn it off for A/B.
6. Chain order: PitchFx → Unison → Slapback → Distortion.

## Engine (`engine/distortion.h`)

```
namespace cv {

struct DistortionTuning {
  float inputHpHz = 20.0f;        // input cap
  // stage 1 shaping (before its saturator)
  float s1BassHz = 700.0f;        // low shelf cut corner
  float s1BassDb = -12.0f;        // low shelf depth below s1BassHz
  float s1LpHz = 3000.0f;         // top roll-off (C5)
  float gain1Max = 100.0f;        // 40 dB
  // tone stack between the stages (bass 10 / mid 6 / treble 0)
  float stackBassHz = 400.0f;     // low shelf boost corner
  float stackBassDb = 10.0f;
  float stackTrebleHz = 2000.0f;  // high shelf cut corner
  float stackTrebleDb = -10.0f;
  float stackLossDb = -20.0f;     // insertion loss
  // stage 2
  float s2HpHz = 100.0f;
  float s2LpHz = 6000.0f;
  float gain2Max = 90.0f;         // just under 40 dB
  float railAsym = 0.05f;         // 0..0.5; positive rail 1, negative -(1 - asym)
  float railSoft = 0.1f;          // soft edge as a fraction of the rail
  // post
  float trebleCutHz = 1000.0f;    // fixed -6 dB high shelf
  float trebleCutDb = -6.0f;
  float toneMinDb = -12.0f;       // TONE fully down: shelf gain added above trebleCutHz
  float toneMaxDb = 6.0f;         // TONE fully up; noon adds 0 dB
  float bassPeakHz = 120.0f;      // gyrator bump
  float bassPeakDb = 6.0f;
  float bassPeakQ = 1.0f;
  float trimDb = 0.0f;
  float fadeDrive = 0.05f;        // dry->chain crossfade span from DRIVE 0
  bool oversample = true;
};

struct DistortionParams {
  bool on = true;                 // menu bypass
  float drive = 0.0f;             // panel knob 0..1
  float tone = 0.5f;              // panel knob 0..1; noon is flat
  DistortionTuning tuning;
};

class Distortion {
 public:
  void reset();
  void process(const float* in, float* out, int n, const DistortionParams& p);
};

}
```

Behaviour per sample (coefficients recomputed per block):

1. `x = hp1(in, inputHpHz)`.
2. Stage 1 shaping: `x = lowShelf(x, s1BassHz, s1BassDb)`; `x = lp1(x, s1LpHz)`;
   `x *= g1`; `x = rail(x)`.
3. Tone stack: `x = lowShelf(x, stackBassHz, stackBassDb)`;
   `x = highShelf(x, stackTrebleHz, stackTrebleDb)`; `x *= dbToLin(stackLossDb)`.
4. Stage 2: `x = hp1(x, s2HpHz)`; `x = lp1(x, s2LpHz)`; `x *= g2`; `x = rail(x)`.
5. Post: `x = highShelf(x, trebleCutHz, trebleCutDb + toneShelfDb)`, where `toneShelfDb` runs
   `toneMinDb`..0 over tone 0..0.5 and 0..`toneMaxDb` over 0.5..1 (`tone` smoothed with the same
   20 ms one-pole as `drive`; settled tone 0.5 equals the pre-TONE sound exactly);
   `x = peakEq(x, bassPeakHz, bassPeakDb, bassPeakQ)`; DC blocker `hp1` at 10 Hz.
6. `rail(x)`: clamp to `[-(1 - railAsym), 1]` with a `tanh` soft edge over the last
   `railSoft` fraction before each rail. Steps 2 and 4 saturators run inside the 2×
   oversampler when `oversample` is on (upsample before `*= g1`, downsample after the
   stage-2 rail; the linear filters in between may run at 2× too, simplest wins).
7. Makeup: `x *= dbToLin(trimDb) / makeup(d)`; implementer picks `makeup(d)` so test 4
   passes and documents the law in a one-line comment.
8. `drive_` one-pole smoothed (20 ms) with snap to exact 0; `on == false` → drive 0.
   Settled drive 0 skips the whole chain and copies input to output exactly. The
   chain's fixed EQ fades in rather than jumping: `out = lerp(in, chain, f)` with
   `f = min(drive_ / fadeDrive, 1)`, `fadeDrive = 0.05` (tuning field). Above that
   the chain is fully in, as on the pedal at any GAIN setting.
9. Shelves and peak are RBJ biquads. Float only, `std::array`, no heap/I/O,
   `-Werror` clean. `tanhf` per sample is fine.

## Emulator

`DISTORTION` checkbox block under SLAPBACK with a `DRIVE` slider `%.0f %%`, opens
at 30 %, then a `TONE` slider `%.0f %%` opening at 50 %. Tuning node `Distortion` with every field above in real units (log sliders
for Hz spanning decades). Reset / Print cover it. `ProtoParams` gains
`cv::DistortionParams distortion`; callback order PitchFx, Unison, Slapback,
Distortion.

## Render CLI

`--drive <0..1>` enables the stage; `--tone <0..1>` sets TONE (default 0.5). Tuning keys via `--tuning k=v`, named
`dist` + field name in lowerCamel (e.g. `distS1BassHz`, `distGain1Max`,
`distRailAsym`, `distToneMinDb`, `distToneMaxDb`, `distTrimDb`, `distOversample`).

## Tests (`test/distortion_test.cpp`, ctest `distortion`)

Test tone = 220 Hz sine at −12 dBFS (amplitude 0.251). Harmonic levels by Goertzel
over seconds 1–2.

1. Passthrough: drive 0, and `on = false` with drive 1 → after 100 ms
   `|out - in| < 1e-6`.
2. Harmonics grow with drive: 3rd harmonic relative to the fundamental at drive 1 is
   ≥ −15 dB, and strictly greater than at drive 0.5, which is greater than at
   drive 0.2.
3. Clean at low drive: at drive 0.15 the 3rd harmonic is ≤ −40 dB relative to the
   fundamental (the pedal "plays clean at low GAIN settings").
4. Level: output RMS within ±4 dB of input RMS at drive 0.15, 0.5, 1.
5. Asymmetry, measured just past the stage-1 clipping onset (about drive 0.45 with
   the defaults), where one rail clips before the other. At full drive the wave is
   a near-symmetric square and the DC blocker removes the only difference; below
   onset nothing clips. At drive 0.5 with default `railAsym` 0.05 the 2nd harmonic
   is ≥ −42 dB relative to the fundamental (measured −39 dB); with `railAsym = 0`
   it is ≤ −55 dB.
6. DC: `|mean(out)|` over seconds 1–2 at drive 1 is < 1e-3.
7. Tone: 8 kHz sine at −12 dBFS, drive 0.5. Output RMS at tone 1 exceeds tone 0 by
   ≥ 12 dB (measured 18 dB), and tone 0.5 is within 0.5 dB of the result with
   `toneMinDb = toneMaxDb = 0` (the pre-TONE sound).
8. No allocation inside `process()`.

## Done when

`ctest` passes all stages; face shows DISTORTION under SLAPBACK; owner listens with
a vocal loop and tunes toward the Blues Driver by ear.
