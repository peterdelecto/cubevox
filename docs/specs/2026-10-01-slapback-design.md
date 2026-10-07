# cubevox — slapback stage

Date 2026-10-01. Owner: "slapback should be 30ms - 120ms. it should have a variable
lowpass filter for the purposes of this prototype." Engine contract, emulator
skeleton, threading unchanged.

## Decisions

1. Panel knob is INTENSITY only. It drives the wet level. Delay time (30–120 ms) and
   the lowpass cutoff are tuning items in the emulator; the owner can promote time to
   the menu later.
2. Single repeat by default. A small feedback tuning value (0–0.5) is there to try a
   second repeat; default 0.
3. Lowpass is a 2-pole Butterworth on the wet path, cutoff tunable 500–12000 Hz,
   default 4 kHz. Coefficients recomputed once per block.
4. Chain order: PitchFx → Unison → Slapback.

## Engine (`engine/slapback.h`)

```
namespace cv {

struct SlapbackTuning {
  float timeMs = 70.0f;        // 30..120
  float lowpassHz = 4000.0f;   // 500..12000
  float feedback = 0.2f;       // 0..0.5 (owner 2026-10-01: a few repeats by default)
  float wetMaxDb = 0.0f;       // wet level at INTENSITY 1
};

struct SlapbackParams {
  float intensity = 0.0f;      // panel knob 0..1
  SlapbackTuning tuning;
};

class Slapback {
 public:
  void reset();
  void process(const float* in, float* out, int n, const SlapbackParams& p);
};

}
```

Behaviour:

1. Delay line `std::array<float, kLen>` with `kLen = 130 ms` of samples. Read with
   Catmull-Rom at `timeMs` (clamped 30–120), the time one-pole smoothed with a 50 ms
   constant so a tuning sweep glides instead of clicking.
2. `wetRaw = readCubic(time)`, `lp = biquadLowpass(wetRaw)`,
   `line[write] = in + feedback * lp`, `out = in + wet * lp` with
   `wet = intensity_ * dbToLin(wetMaxDb)`. `intensity_` is one-pole smoothed (20 ms)
   with snap to exact 0, so INTENSITY 0 is sample-exact passthrough once settled.
3. Biquad: standard RBJ lowpass, Q = 0.7071, cutoff clamped 500–12000 Hz.
4. No heap, no I/O, float only, `-Werror` clean.

## Emulator

`SLAPBACK` block under UNISON: `INTENSITY` slider `%.0f %%`, opens at 50 %. Tuning
gets a `Slapback` node: Time (30–120 ms), Lowpass (500–12000 Hz), Feedback (0–0.5),
Wet level at full (−24–0 dB). Reset / Print cover it. `ProtoParams` gains
`cv::SlapbackParams slapback`; callback order PitchFx, Unison, Slapback.

## Render CLI

`--slap <0..1>` enables the stage at that intensity. Tuning keys via the existing
`--tuning k=v` mechanism: `slapTimeMs`, `slapLowpassHz`, `slapFeedback`, `slapWetMaxDb`.

## Tests (`test/slapback_test.cpp`, ctest `slapback`)

1. Passthrough: 1 s 440 Hz sine, intensity 0 → after 100 ms `|out - in| < 1e-6`.
2. Time: unit impulse at t = 0.5 s, intensity 1, lowpass 12 kHz, feedback 0 → wet
   peak (`out - in`) lands within ±2 samples of 80 ms (test sets timeMs 80 and feedback 0 explicitly) after the impulse; with
   `timeMs = 30` and `120` the same holds for those times (fresh reset each run).
3. Lowpass: 2 s sines at 500 Hz and 8 kHz, intensity 1, cutoff 4 kHz, feedback 0 →
   wet RMS at 8 kHz is 9–15 dB below wet RMS at 500 Hz (one octave above a 2-pole
   cutoff, nominal −12 dB).
4. Feedback: impulse, feedback 0.5 → second repeat at 160 ms with peak 0.5 ± 0.1 of
   the first.
5. No allocation inside `process()`.

## Done when

`ctest` passes `unison`, `harmony`, `octave`, `slapback`, `proto_layout`; face shows
SLAPBACK under UNISON; owner listens.

Level rule (2026-10-01, see `2026-10-01-level-rule.md`): `wetMaxDb` default is now 4.9 dB (was 0), set so INTENSITY 0.5 reads +0.27 dB out/in.

Level rule correction (owner 2026-10-07): that +0.27 dB was luck. The test tone at 180 Hz
cancels against its own echo at 70 ms and adds at 100 ms (+6.3 dB), so one TIME is a coin
flip. `level_test` now averages out/in over TIME 60..250 ms in 10 ms steps, which is the
power sum; with `wetMaxDb` kept at 4.9 dB that mean is +2.70 dB. As with Unison, the wet
stays the effect and a whole-output `trimDb` = -4.9 dB x INTENSITY brings the mean to
+0.25 dB. INTENSITY 0 stays bit-exact unity.

## INTENSITY and TIME on the panel (owner 2026-10-02)

1. INTENSITY sets the wet level and the repeats together. Feedback =
   min(0.5, tuned feedback x sqrt(INTENSITY / 0.25)): the owner's 0.35 (about 2.5 repeats)
   lands at the default 25 %, grows with the knob and caps at 0.5 from about 51 %.
2. TIME is the second panel knob, 60..250 ms (default 100; Adam never slaps under 70 ms,
   owner 2026-10-07). The delay line holds 260 ms.
3. `slapback_test` checks the repeat ratio at 6 / 25 / 100 % against the law and peak
   timing at 60 / 100 / 180 / 250 ms.
