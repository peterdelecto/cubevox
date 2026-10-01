# cubevox — spring reverb stage

Date 2026-10-01. Owner: "let's do spring. the spring reverb on drumsynthv3 sounds
pretty good but not sploshy enough." Panel knobs TENSION and DWELL, no mix (owner
2026-10-01). Engine contract, emulator skeleton, threading unchanged.

Algorithm reference (read-only; DrumSynthV3 is frozen):
`/Users/j/Documents/Claude/Projects/DrumSynthV3/firmware/DrumSynthV3/audio_effect_spring_v3.h`
and `firmware/RESEARCH_SPRING_REVERB_2026-09-02.md`. The model is Välimäki /
Parker / Abel, "Parametric Spring Reverberation Effect" (JAES 2010) plus Parker
(EURASIP 2011). Write the cubevox version fresh in cubevox style; same algorithm.

## What the model is

Per spring, the low-frequency chirp block C_lf: input summer → 40 Hz DC blocker →
M = 130 stretched first-order allpasses `H(z) = (a + z^-K) / (1 + a z^-K)`,
`a = +0.75` (positive: lows arrive first, the chirp sweeps UP) → multitap delay of
lap L with pre-echo ripple taps at L/5 and +1 ms (gains 1 : r : r : r²,
normalised to unit sum) → feedback `g` (negative) back to the summer. The cascade
sits INSIDE the loop so echo n carries n·M sections of dispersion. Output = tap
before the delay → 6th-order elliptic low-pass at `F_C = fs / 2K`. The high band
C_hf: 2nd-order Butterworth high-pass at F_C → delay L/2.3 → feedback `0.77/0.80 ·
g`, cross-coupled 0.1 into the C_lf summer, mixed into the output at `hfMix`. The
delay read is wandered by filtered noise (±8 samples, ~3 Hz). Two detuned springs
summed ×0.5. Front end shared: RC high-pass ~300 Hz → DWELL drive into a soft clip
(Padé tanh clamped at |x| = 3) with level compensation `drive^-comp` → trim.

## Why it is not sploshy, and the levers

The research doc's own definition: splash = the fast non-dispersive band above F_C,
plus the pre-echo ripple ahead of each chirp, plus driver saturation at high
DWELL. DrumSynthV3 keeps all three modest (C_hf at −22…−14 dB, ripple 0.1, drive
32×). Levers, all tuning items here:

1. `hfMixDbLo / hfMixDbHi` — C_hf level at DWELL 0 / 1. Default −18 / −8 (was
   −22 / −14).
2. `rippleGain` — pre-echo tap gain r. Default 0.2 (paper 0.1).
3. `splashDiffuse` — two short Schroeder allpasses (3.1 ms and 7.3 ms, both
   coefficient `splashDiffuse`) on the C_hf path before its delay, so the bright
   attack smears into a splash instead of a slap. 0 = off. Default 0.5.
4. `dwellDrive / dwellComp` — DWELL law `drive = dwellDrive^x`, out × `drive^-comp`.
   Default 32 / 0.8.
5. `hfSections` — C_hf allpass cascade length (paper 200, DSV3 0). Default 0;
   range 0–200, adds top-octave dispersion to the splash at ~1 MAC/section.
6. `springs` — 2 or 3. Third spring K = 7 (F_C 3.43 kHz, lap 48 ms). Default 2.
7. `hpHz` — front-end high-pass, default 300. `boingDb` — the paper's 95 Hz /
   BW 130 Hz resonator on the output, default 0 (off) because the high-pass sits
   above it; lower `hpHz` first if trying it.
8. `modDepth / modRateHz` — wander, default 8 samples / 3 Hz.
9. `wetDb` — wet level, default −6 dB (no mix knob on the panel). Dry passes at
   unity.

## Geometry at 48 kHz

Lap the ear hears = L + cascade low-band group delay `M·K·(1−a)/(1+a) = M·K/7`.

| Spring | K | F_C | lap | L = lap − M·K/7 |
|---|---|---|---|---|
| A | 5 | 4800 Hz | 40 ms = 1920 | 1827 |
| B | 6 | 4000 Hz | 44 ms = 2112 | 2001 |
| C | 7 | 3429 Hz | 48 ms = 2304 | 2174 |

Guard 32 samples on each delay for the wander. Pre-echo tap at L/5, ripple +48
samples (1 ms). C_hf delay L/2.3.

Elliptic band-limit tables (SOS rows `{b0, b1, b2, a1, a2}`, a0 = 1; the
normalised cutoff is fs-independent so K = 5, 6 match DSV3's):

```
// K = 5: scipy.signal.ellip(6, 0.5, 60, 4800, fs=48000, output='sos')
{3.170187928e-03f, 2.448448052e-03f, 3.170187928e-03f, -1.574285048e+00f, 6.496029638e-01f},
{1.000000000e+00f, -9.573141810e-01f, 1.000000000e+00f, -1.551997326e+00f, 7.965495053e-01f},
{1.000000000e+00f, -1.285106260e+00f, 1.000000000e+00f, -1.562862547e+00f, 9.396148292e-01f},
// K = 6: scipy.signal.ellip(6, 0.5, 60, 4000, fs=48000, output='sos')
{2.383943886e-03f, 1.007344239e-03f, 2.383943886e-03f, -1.646843207e+00f, 7.000183821e-01f},
{1.000000000e+00f, -1.226384803e+00f, 1.000000000e+00f, -1.652930569e+00f, 8.257039330e-01f},
{1.000000000e+00f, -1.484337949e+00f, 1.000000000e+00f, -1.682745577e+00f, 9.483735958e-01f},
// K = 7: scipy.signal.ellip(6, 0.5, 60, 3429, fs=48000, output='sos')
{1.954320361e-03f, 2.113410835e-04f, 1.954320361e-03f, -1.698075320e+00f, 7.376835735e-01f},
{1.000000000e+00f, -1.407211516e+00f, 1.000000000e+00f, -1.719214914e+00f, 8.477613502e-01f},
{1.000000000e+00f, -1.612118776e+00f, 1.000000000e+00f, -1.757949845e+00f, 9.550323535e-01f},
```

## Engine (`engine/spring.h`)

```
namespace cv {

struct SpringTuning {
  float hpHz = 300.0f;
  float tensionLo = 0.60f, tensionHi = 0.97f;   // |g| at TENSION 0 / 1
  float dwellDrive = 32.0f, dwellComp = 0.80f;
  float hfMixDbLo = -18.0f, hfMixDbHi = -8.0f;  // C_hf mix at DWELL 0 / 1
  float rippleGain = 0.20f;                     // pre-echo taps, paper 0.1
  float splashDiffuse = 0.50f;                  // 0 off .. 0.9
  int   hfSections = 0;                         // 0..200
  int   springs = 2;                            // 2 or 3
  float modDepth = 8.0f, modRateHz = 3.0f;
  float boingDb = 0.0f;                         // 95 Hz resonator, 0 = off
  float wetDb = -6.0f;
  float tankTrim = 0.375f;                      // DSV3: inTrim 0.25 x tankTrim 1.5
};

struct SpringParams {
  bool on = true;
  float tension = 0.5f;   // panel knob 0..1
  float dwell = 0.5f;     // panel knob 0..1
  SpringTuning tuning;
};

class Spring {
 public:
  void reset();
  void process(const float* in, float* out, int n, const SpringParams& p);
  // Test hooks.
  void setModEnabled(bool on);
  void setSolo(int spring);   // -1 = all
};

}
```

Behaviour:

1. All float, `std::array` storage sized for three springs (allpass states
   `130 × K`, delay `L + 32`, C_hf delay `L/2.3 + 8`, C_hf cascade 200). About
   60 KB total; fine on the H7.
2. Per block: compute `g = -(tensionLo + tension·(tensionHi − tensionLo))`,
   `drive = dwellDrive^dwell`, `comp = drive^-dwellComp`, `hfMix` from the dB
   interpolation on `dwell`, `wet = dbToLin(wetDb)`. Smooth `tension`, `dwell`
   and the on/off `active` with 20 ms one-poles; `active` snaps to exact 0 so
   settled off is sample-exact passthrough. Tanks keep running while off so
   turning on does not start from silence; they are only not mixed.
3. Per sample: front end `hp1 → ×drive → softClip → ×comp × tankTrim`; each
   enabled spring `process(x)` as in the reference, with the splash diffusers on
   the C_hf input and `hfSections` allpasses after them; sum ÷ `springs`;
   optional boing resonator; `out = in + active · wet · sum`.
4. Wander noise per spring from independent xorshift seeds; `setModEnabled(false)`
   freezes the read length so test arrival times are exact.
5. `softClip` is the Padé form `x(27 + x²)/(27 + 9x²)` clamped at |x| = 3.
6. Delay reads linear-interpolated; rounding not needed (float lines).
7. `-Werror` clean, no heap/I/O in `process()`.

## Emulator

`SPRING` checkbox block under DISTORTION with `TENSION` and `DWELL` sliders
`%.0f %%`, both opening at 50 %. Tuning node `Spring` with every field above in
real units; `springs` and `hfSections` as int sliders. Reset / Print cover it.
`ProtoParams` gains `cv::SpringParams spring`; callback order PitchFx, Unison,
Slapback, Distortion, Spring. Face height is already 910 px: if adding the block
pushes past ~980 px, move Tuning into a second column instead of growing taller.

## Render CLI

`--spring` enables the stage, `--tension <0..1>`, `--dwell <0..1>`. Tuning keys
`spr` + field name in lowerCamel (`sprHpHz`, `sprRippleGain`, `sprSplashDiffuse`,
`sprHfSections`, `sprSprings`, `sprWetDb`, …).

## Tests (`test/spring_test.cpp`, ctest `spring`)

Wet-only measurements use `wetDb = 0` and subtract the dry input. Impulse tests
use `setModEnabled(false)`, `setSolo(0)`, tension 0.5, dwell 0.

1. Passthrough: `on = false` → after 100 ms `|out − in| < 1e-6`.
2. Chirp runs up: unit impulse; band-pass the wet output at 500 Hz and 4 kHz
   (2nd-order, Q 4); the first envelope peak (first local maximum within 3 dB of
   the window maximum, zero-phase 10 ms smoothing) of the 4 kHz band arrives at
   least 10 ms AFTER the 500 Hz band's within the first lap (0–60 ms). Analytic
   cascade group delay for spring A is 2 ms at 500 Hz and ~22 ms at 4 kHz; 3.5 kHz
   was too close to the 500 Hz band-pass ringing to resolve.
3. Echo train: envelope maxima (10 ms smoothing, > −40 dB re the first) of the
   wet impulse response in 0–500 ms number between 4 and 40.
4. Tension sets decay: impulse responses at tension 0 and 1; RMS of 1.0–1.5 s
   relative to RMS of 0–0.5 s is at least 20 dB higher at tension 1 than at 0.
5. Band limit of the chirp path: dwell 0, white noise 2 s, with the high band
   muted (`hfMixDbLo = hfMixDbHi = -120`); wet energy above 7.2 kHz (1.5 F_C of
   spring A) is ≥ 40 dB below wet energy in 300 Hz–4 kHz (measured 56 dB). With
   the high band at its default the same ratio is only ~7 dB, which is the splash
   by design and is covered by check 6, not a defect.
6. Splash grows with dwell: same noise; wet energy ratio (above 5 kHz / below
   4 kHz) at dwell 1 exceeds that at dwell 0 by ≥ 6 dB.
7. Stable: tension 1, dwell 1, all springs, 3 s of white noise at −6 dBFS then
   3 s silence; no NaN/inf, |out| < 4 throughout, RMS of the last 0.5 s below
   RMS of the first 0.5 s of silence.
8. No allocation inside `process()`.

## Done when

`ctest` passes all stages; face shows SPRING under DISTORTION; owner listens and
reports whether it is sploshy enough, then the levers get baked.
