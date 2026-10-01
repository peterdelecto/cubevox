# cubevox — reverb option C: parametric spring from the research handover

Date 2026-10-01. Owner: "we will build one more option for reverb, C, from this spec"
(`docs/reference/spring-reverb-research.md`). Reverb block gains a third engine,
labelled **PARKER** in the prototype (SPRING = DrumSynthV3 port, CHASM = hexefx port).
Panel knobs TENSION / DWELL / MIX apply to all three.

## What C is, and how it differs from A

Both A and C are the Välimäki/Parker/Abel parametric model. A is DrumSynthV3's
realisation: integer stretch K, a = +0.75, M = 130, no chirp EQ, 6th-order elliptic
at fs/2K, DSV3's taps and wander, 6G15 front end. C follows the handover's build plan
milestone 3 and the DAFx-11 "automated, M_low ≤ 100" column, then milestone 5 and 6:

1. **Fractional stretch** so F_c is exact at 48 kHz (handover §2.2): K = fs/(2 F_c,lf),
   K1 = round(K) − 1, d = K − K1, a2 = (1 − d)/(1 + d); each section is a first-order
   allpass (a1) wrapping a K1 delay followed by the fractional allpass A_fd(a2).
2. **Chirp EQ** H_eq (stretched resonator) on the output path.
3. **10th-order elliptic** H_low (1 dB ripple, 60 dB stop, 4750 Hz) as 5 SOS.
4. **Gajarsky multitap and modulation**, DAFx-11's 1.2 compensation on g_lf.
5. **C_hf** M = 189, a = −0.34, g_hf = 1.3·g_lf, cross-couple c1 = 0.1, mixed at
   g_high = g_low/1000 by default (tunable; A's −22 dB is the other extreme).
6. **Three detuned springs** with ÆLAPSE-style factors on T_D and F_c (tunable; the
   handover's factors are wide, defaults below are milder), interleaved in one loop.
7. **Drive/recovery wrapper** (handover §7.3 stand-in): HPF 150 Hz → DWELL drive into
   soft clip with level compensation → LPF 6 kHz → tank → presence peak +3 dB at 3 kHz.
8. Feedback sign resolved by test: the first echo must be polarity-inverted.

Multirate (Parker 2011 §3) is NOT built in C; it is the H7 cost optimisation and
stays a later step. The Mac prototype runs full rate.

## Panel mapping

- TENSION → decay. `g_lf = −(gLo + x·(gHi − gLo))`, defaults 0.45..0.90 (DAFx-11's
  0.537 raw, ×1.2 = 0.64, sits at x ≈ 0.42). `g_hf = 1.3·g_lf` clamped to |g| ≤ 0.97.
- DWELL → drive, same law as A: `drive = dwellDrive^x`, `comp = drive^-dwellComp`.
- MIX → handled by `Reverb` (equal-power dry/wet), engine outputs wet at 0 dB.

## Engine (`engine/spring_c.h`)

```
struct SpringCTuning {
  float tdMs = 56.0f;            // T_D, spring 1
  float fcLfHz = 4275.0f;        // F_c,lf, sets K (fractional)
  int   mLow = 100;              // 50..100 (array sized 100)
  float aLf = 0.63f;
  float gLo = 0.45f, gHi = 0.90f;      // |g_lf| at TENSION 0 / 1 (before ×1.2 comp)
  float gComp = 1.2f;                  // DAFx-11 modulation/interp compensation
  float hfRatio = 1.3f;                // g_hf / g_lf
  int   mHigh = 189;             // 0..200 (0 disables C_hf)
  float aHf = -0.34f;
  float hfMixDb = -60.0f;        // g_high re g_low; A uses -22
  float cross = 0.1f;            // c1
  float eqPeakHz = 183.0f, eqBwHz = 146.0f;   // chirp EQ (DAFx-11); 0 bw = off
  float lowHz = 4750.0f;         // H_low cutoff (table is fixed; field documents it)
  float echoGain = 0.1f, rippleGain = 0.1f;  // taps 1 : r : e : r·e at L0+{echo,ripple}
  float modDepth = 8.0f, modPole = 0.93f;    // Gajarsky
  int   springs = 3;             // 1..3
  float tdFactor[3]  = {1.0f, 1.09f, 0.94f};   // ÆLAPSE-style detune (milder)
  float fcFactor[3]  = {1.0f, 0.98f, 1.02f};
  float hpHz = 150.0f, lpHz = 6000.0f;       // drive/recovery wrapper
  float dwellDrive = 32.0f, dwellComp = 0.80f;
  float presenceHz = 3000.0f, presenceDb = 3.0f, presenceQ = 1.0f;
  float tankTrim = 0.75f;        // into the tank, after the clip
};

struct SpringCParams {
  float tension = 0.5f;
  float dwell = 0.5f;
  SpringCTuning tuning;
};

class SpringC {
 public:
  void reset();
  void process(const float* in, float* wet, int n, const SpringCParams& p);  // wet only, 0 dB
  // Test hooks
  void setModEnabled(bool on);
  void setSolo(int spring);   // -1 = all
};
```

Behaviour per spring (handover §2.3, Parker simplified):

1. Geometry at 48 kHz: `K = fs/(2·fcLf·fcFactor)`, `K1 = round(K) − 1`, `a2 = (1 − d)/(1 + d)`.
   `τ_DC = K·mLow·(1 − aLf)/(1 + aLf)`. `L = round(tdMs·tdFactor·fs/1000 − τ_DC)`.
   `L_echo = round(L/5)`, `L_ripple = round(K)` (2K·0.5), `L0 = L − L_echo − L_ripple`.
   `L_hf = round(L/2.3)`. Worked spring 1: K = 5.614, K1 = 5, a2 = 0.239, τ_DC = 127,
   L = 2561, L_echo = 512, L_ripple = 6, L0 = 2043, L_hf = 1113.
2. Per sample: `s = x·tankTrim − g_lf·d + cross·hfOut` where `d` is the multitap read
   (taps at L0, L0+L_ripple, L0+L_echo, L0+L_echo+L_ripple, gains e·r, e, r, 1, read
   with linear interpolation at `+ modDepth·noise`, noise = one-pole(pole) of white).
   The sign convention `s = x − g·d` with g negative (Gajarsky) is the starting point;
   if the first-echo polarity test fails, flip to `+ g·d`. Record which in a comment.
3. `u = H_dc(s)` (40 Hz allpass-based HP), then the M stretched sections
   (`y = a1·v + w; w_new = v − a1·y` where the K1-delayed state passes through
   A_fd(a2) before use). State per section: K1-deep ring + A_fd state.
4. Write `u` to the loop delay. Output path: `H_eq(u)` (stretched resonator, K_eq =
   floor(K)) → `H_low` (5 SOS, table below) → `lfOut`.
5. C_hf: `sh = x·tankTrim − g_hf·dh`, `mHigh` plain first-order allpasses (a = aHf),
   write to a delay of `L_hf`, output at the node → `hfOut`.
6. Spring output `lfOut + dbToLin(hfMixDb)·hfOut`.

Whole engine: front end `hp1(hpHz) → ×drive → softClip (Padé) → ×comp → lp1(lpHz)`;
sum of enabled springs ÷ springs; `peakEq(presenceHz, presenceDb, presenceQ)`; out.
Smooth tension and dwell 20 ms. All float, `std::array`, sized for 3 springs × 100
sections (K1 ≤ 8 → ring 8 per section) + 200 hf sections + delays (≤ 3200 + guard).

H_low table (48 kHz), rows `{b0, b1, b2, a1, a2}`:

```
// scipy.signal.ellip(10, 1, 60, 4750, fs=48000, output='sos')
{2.347440819e-03f, 9.108687622e-04f, 2.347440819e-03f, -1.686439625e+00f, 7.363054338e-01f},
{1.000000000e+00f, -1.246292218e+00f, 1.000000000e+00f, -1.656426097e+00f, 8.467574505e-01f},
{1.000000000e+00f, -1.515232208e+00f, 1.000000000e+00f, -1.632407455e+00f, 9.363177933e-01f},
{1.000000000e+00f, -1.582644451e+00f, 1.000000000e+00f, -1.622367722e+00f, 9.777837304e-01f},
{1.000000000e+00f, -1.601133904e+00f, 1.000000000e+00f, -1.621570351e+00f, 9.947415935e-01f},
```

### `engine/reverb.h`

`ReverbParams` gains `SpringCParams parker;` and `kReverbParker = 2`. `Reverb` owns
a `SpringC`, processes the selected engine, resets the others on switch, applies MIX.

## Emulator / render

REVERB radio becomes `SPRING | CHASM | PARKER`. Tuning header gains a `Parker`
sub-node (split into "Tank" / "Springs" / "Drive" if taller than the column). Render:
`--reverb parker`; tuning keys `prk` + field name.

## Tests (`test/spring_c_test.cpp`, ctest `spring_c`)

Impulse tests use `setModEnabled(false)`, `setSolo(0)`, dwell 0, wrapper HPF/LPF left
in. Handover §9.3 validation, made executable:

1. T_D: autocorrelation of the wet IR (0–300 ms) has its largest off-zero |peak| at
   56 ± 1 ms.
2. First echo polarity: the sample of largest |value| in the window 40–70 ms has the
   opposite sign to the direct-path peak in 0–10 ms. (If this fails with `x − g·d`,
   switch the sign and document.)
3. F_c: chain group delay from the chain-alone impulse response (expose a test hook
   `chainImpulse(spring, buf, n)`), measured as the frequency of the latest-arriving
   energy via a 2048-point STFT ridge: maximum within 4275 ± 150 Hz.
4. Decay per pulse: Schroeder EDC at pulse positions k·T_D for tension at the value
   that gives |g_lf|·1.2 = 0.64 (x ≈ 0.42); slope −5.4 ± 1.0 dB per pulse over the
   first six pulses.
5. Tension: late/early RMS gap between tension 0 and 1 ≥ 20 dB.
6. Band limit: white noise, dwell 0, `hfMixDb = -120`; wet energy above 7 kHz ≥ 45 dB
   below 300 Hz–4 kHz.
7. Three springs detune: with `springs = 3`, the IR autocorrelation shows three
   distinct peaks within 45–65 ms (T_D·{0.94, 1.0, 1.09} = 52.6/56/61 ms ± 1).
8. Stable: tension 1, dwell 1, 3 springs, 3 s white noise at −6 dBFS then 3 s silence;
   no NaN/inf, |out| < 4, decaying.
9. Passthrough through `Reverb` with engine 2 and `on = false`; no allocation.

## Done when

`ctest` passes; REVERB offers three engines; owner A/Bs SPRING vs PARKER on a vocal
loop.
