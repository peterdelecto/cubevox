# Spring reverb DSP: implementation specification for Cortex-M7

Owner-supplied research handover, received 2026-10-01. Reproduced verbatim as the
reference for reverb option C (`docs/specs/2026-10-01-spring-c-design.md`). Tags:
**[cited]** from a named source; **[derived]** computed from cited material;
**[unverified]** second-hand.

Papers not read in full: Välimäki/Parker/Abel JAES 2010, Välimäki/Abel/Smith JAES
2009, Abel/Berners/Costello/Smith AES 2006, Vienna Talk 2022 (abstract only).

## 0.1 Recommended architecture (summary)

Build the Välimäki/Parker/Abel parametric model in Parker's 2011 multirate form, one
instance per spring, two or three detuned springs in parallel, wrapped in a nonlinear
drive/recovery stage.

| Approach | What it gives | Runtime cost | Live parameters | M7 verdict |
|---|---|---|---|---|
| FDTD (Bilbao & Parker 2010, Bilbao 2013) | Physical, all chirps | 18–160× slower than real time in MATLAB | yes | no |
| Modal bank (van Walstijn 2020; McQuillan 2021, 2025) | Most accurate published | 1,009 / 2,031 / 3,835 resonators | no | truncated only |
| Parametric stretched-allpass (JAES 2010) | Main chirps, echoes, decay | ~1,023 multiplies/sample | yes | yes, one spring |
| Parker 2011 multirate variant | Same, slight echo "hardness" | ~355 multiplies/sample | yes | yes, 2–3 springs |
| Abel–Smith designed biquad chain | Fits a measured group delay | ~20 biquads per spring | coefficient interpolation only | yes; non-parametric |
| ChowDSP/Surge nested-Schroeder loop | Spring-flavoured, cheap | ~250 multiplies per mono sample | yes | good first milestone |
| Convolution (Befaco) | Exact LTI snapshot | 5.6 s IR, partitioned FFT | no | feasible, inflexible |

Budget: 13,605 cycles/sample at 600 MHz or 10,884 at 480 MHz at 44.1 kHz [derived].

## 1. Spring physics (summary of the handover's §1)

- `T_D ≈ 4 L R / (r · sqrt(E/ρ))` echo spacing; `f_C ≈ 3 r sqrt(E/ρ) / (16 √5 π R²)`
  transition frequency [cited: Parker & Bilbao DAFx-09 Eq. 2, 4].
- Steel E = 2.11e11 Pa, ρ = 7800 → c = 5201 m/s.
- Measured tanks give T_D 30–55 ms, f_C 2.5–5 kHz. Leem KA-1210 spring 1 calibrated
  (DAFx-11): T_d 56 ms, F_c 4216–4300 Hz.
- Damping: no first-principles model; `σ₀ ≈ 3 s⁻¹` at LF (T60 ≈ 2.3 s), rising with
  frequency (`σ_i = σ₂ ω_i² + σ₀`, σ₂ = 3e-9 s).

## 2. The parametric dispersive-allpass model

### 2.1 Building blocks

```
A(z^K) = (a + z^-K) / (1 + a z^-K)             stretched first-order allpass
H(z)   = A^M(z^K)                              chain of M
D(ω)   = K·M·(1 − a²) / (1 + 2a cos(ωK) + a²)  group delay (samples)
K      = fs / (2 f_C)                          (a > 0)
τ_DC   = K·M·(1 − a)/(1 + a)                   minimum (DC) group delay
τ_max  = K·M·(1 + a)/(1 − a)                   peak at f_C
```

### 2.2 Non-integer stretch

Embed a first-order fractional-delay allpass inside the stretched section:
`A_M(z) = [(a1 + A_fd(z) z^-K1) / (1 + a1 A_fd(z) z^-K1)]^M`,
`A_fd(z) = (a2 + z^-1)/(1 + a2 z^-1)`, `K1 = round(K) − 1`, `d = K − K1`,
`a2 = (1 − d)/(1 + d)` (Gajarsky). Code [12] structurally: A_fd whose input is the
K1-delayed state, inside a first-order allpass with coefficient a1 (4 multiplies).

Worked at 48 kHz, f_C,lf = 4275: K = 5.614, K1 = 5, d = 0.614, a2 = +0.239.

### 2.3 Signal flow of C_lf (DAFx-11 Fig. 1, Parker simplified)

```
s[n] = x[n] − g_lf · d[n]            (sign convention: see below)
u    = H_dc { s }  →  A_low^M(z^K) { · }
out  = H_eq → H_low { u }
d    = multitap delay of u (length from T_D), read position modulated
```
- `H_dc`: 40 Hz high-pass (first-order allpass-based: `adc = tan(π/4 − π·40/fs)`,
  `y1 = 0.5(1 + adc)(x − x1) + adc·y1`).
- `H_eq` chirp EQ: second-order resonator stretched by K_eq = floor(K). Gajarsky:
  `f_peak = 95, B = 130, R = 1 − π B K_eq/fs, p_cos0 = ((1+R²)/(2R)) cos(2π f_peak K_eq/fs),
  A0 = 1 − R², aeq1 = −2R p_cos0, aeq2 = R²,
  y3[n] = (A0/2)(x[n] − x[n−2K_eq]) − aeq1 y3[n−K_eq] − aeq2 y3[n−2K_eq]`.
  DAFx-11 fit: F_peak 183 Hz, B 146 Hz, K 5.
- `H_low`: 10th-order elliptic, 1 dB ripple, 60 dB stop, 4750 Hz, as 5 SOS (never
  direct form in float32).
- Multitap (Gajarsky): `L = round(T_d fs − K M (1−a1)/(1+a1) + g_mod·noise)`,
  `L_echo = round(L/5)`, `L_ripple = round(2 K · 0.5)`, `L0 = L − L_echo − L_ripple`,
  taps at L0, L0+L_ripple, L0+L_echo, L0+L_echo+L_ripple with gains
  0.01, 0.1, 0.1, 1 (g_echo = g_ripple = 0.1).
- Modulation: one-pole-filtered white noise, pole 0.93, depth 8 samples; multiply
  g_lf by 1.2 to compensate modulation + linear interpolation loss (DAFx-11).
- C_hf: `Mh = 200` plain first-order allpasses (K = 1), `ah = −0.6` (DAFx-11 manual) or
  189 / −0.34 (automated); loop delay `Lh = round(L/2.3)`; `g_hf = −0.77` (manual) or
  −0.83 (= 1.3·g_lf); no EQ, no lowpass.
- Cross-couple `c1 = 0.1` (hf output into lf input), `c2 = 0`. Output
  `g_low = 0.97, g_high = g_low/1000, g_dry = 1 − g_low − g_high`. Parker: wide-band
  chirps are ≥ 30 dB below the lf chirps.
- Loop length: `D_loop = round(T_D fs − N_chains τ_DC − τ_fixed)`, N_chains = 1.
- **Feedback sign unresolved.** Figures draw "−" at the adder AND list negative g;
  Gajarsky applies `x − g·yf` with g = −0.8 (net positive). Resolve empirically: the
  first echo must be polarity-inverted relative to the input (DAFx-11 §2.5).

### 2.4 Published parameter sets (DAFx-11 Table 1, Leem KA-1210 spring 1, fs 44.1 kHz)

| Parameter | Manual (JAES 2010) | Automated | Automated, M_low ≤ 100 |
|---|---|---|---|
| T_d (s) | 0.056 | 0.056 | 0.056 |
| F_c (Hz) measured | 4300 | 4216 | 4216 |
| F_c,lf (Hz) sets K | 4300 | 4980 | 4275 |
| M_low | 100 | 318 | 100 |
| a_1 (lf) | 0.62 | 0.69 | 0.63 |
| g_lf | −0.8 | −0.64 | −0.64 |
| M_high | 200 | 189 | 189 |
| a_high | −0.6 | −0.34 | −0.34 |
| g_hf | −0.77 | −0.83 | −0.83 |

EDC slope −5.4 dB per pulse. Other variant ("Fifty Years" Fig. 18): F_c 4 kHz,
T_D 50 ms, velvet-noise diffusion in the feedback, g_fb = −1.4.

### 2.5 Calibration (DAFx-11 §3)

T_D = lag of |autocorrelation| maximum; F_c = frequency of max mean per-bin ACF;
g_lf from Schroeder EDC slope per pulse `d = m T_D`, `g = sgn(max ACF)·10^(d/20)·1.2`;
M, a, K by NLS fit of the chain group delay to spectrogram ridges; chirp EQ by NLS
fit to the first chirp's spectrum; C_hf from the periodicity of spectrogram ACF
peaks, `g_hf = 1.3 g_lf`.

### 2.6 Operation counts (Parker 2011 Table 2)

Original 1023 multiplies/sample (C_lf chain 600, C_hf chain 400, H_low 18, mixes);
multirate 355 (C_lf chain at fs/4: 75; C_hf stretch 2: 200; crossovers 34;
anti-alias 20; aligning delays 4). Dropping C_hf leaves ≈ 126–129.

## 3. Parker 2011 multirate structure

- Chirp straightening: `k → 2k, a → −a` puts a group-delay maximum at DC, keeps the one
  at f_C, minimum at f_C/2; the band below f_C/2 bypasses the chain through a
  time-aligning delay; M/2 sections. Audible as slight echo "hardness".
- C_lf at fs/4: decimate with Chebyshev-I order 10, 2 dB ripple; 8th-order
  Linkwitz–Riley at f_C/2; high band → `A^(M/2)(z^(k/2))` with −a; low band →
  aligning delay (integer + first-order allpass fractional via
  `a = [−D cos ω ± sqrt(1 − D² + D² cos² ω)]/(1 + D)`); sum → loop delay → g → adder;
  interpolate ×4 → H_low. Lagrange read for the modulated delay.
- C_hf: stretch 2, a unchanged, M/2 sections; 4th-order L-R at fs/4; low band through
  the chain, high band through an aligning delay (negative-a root).

## 4. Abel & Smith designed dispersion filters

Allpass pair from pole `ρ e^{jθ}`: `τ(ω) = (1 − ρ²)/(1 + ρ² − 2ρ cos(ω − θ))`, area
2π per section. Algorithm: add constant delay so ∫δ = 2πN; divide the axis into
bands of area 2π; fit one section per band with `θ = (ω₊+ω₋)/2`,
`ρ = η − sqrt(η² − 1), η = (1 − β cos Δ)/(1 − β)`. Spring example: 20 biquads.
UA patent US 8,391,504 B1 (active to 2031-11-05): waveguide per propagation mode
with dispersion allpass D(z) and attenuation A(z); every independent claim requires
more than one mode. Personal project only.

## 5. Modal models

Runtime: bank of second-order resonators `y_j[n+1] = a_j y_j[n] + b_j y_j[n−1] + c_j u[n]`,
`a_j = 2 e^(−α dt) cos(ω dt)`, `b_j = −e^(−2α dt)`. 1009 (2020), 2031 (2021), 3835
(2025) modes; offline eigen-solve; no live parameters. chowdsp ModalSpringReverb:
209 modes peak-picked from a recorded IR (GPLv3).

## 6. Reference code (summaries)

- **Surge/ChowDSP**: loop = delay → fasttanh → 40 Hz HP → 16 nested Schroeder allpass
  stages (D_ap = 0.35 + 3.0·size ms, g = 0.5 − 0.4·spin, lanes ±g) → LP
  `4000·(18000/4000)^(1−damp)`; `D = 1000 + 0.099·size·fs`, `g_fb = 0.001^(D/(T60 fs))`,
  `T60 = 0.5·9^(0.95 d − 0.7(1 − s²))`; chaos = random delay lengthening; reflection
  network of four delays {0.07, 0.17, 0.23, 0.29} s·size. GPL-3.
- **ÆLAPSE Springs**: four SIMD springs, detune `kFreqFactor {0.98, 1.02, 0.97, 1.03}`,
  `kRFactor {1.08, 0.97, 1.05, 0.98}`, `kLoopTdFactor {0.829, 1.188, 0.943, 1.243}`,
  loop LFO 0.2–0.4 Hz depth 0.002–0.0038; up to 160 second-order allpasses per
  spring; variable decimation fs/rate (rate ≤ 8) behind a 15-tap FIR; loop:
  predelay, cubic read, Householder across springs, `tanh(0.2x)/0.2`, 10 Hz DC block;
  output band-pass SVF + 10th-order Chebyshev-I. GPL-3.
- **hexefx**: the CHASM engine (already in cubevox). MIT.
- **Befaco**: 5.577 s recorded IR convolution; EDC −10/−20/−30 dB at 0.49/1.10/2.45 s;
  energy peaks 200–400 Hz, falls 16 dB from 1.6 to 6.4 kHz. Tuning target only.

## 7. Nonlinearity, drive, transducer

No published nonlinear spring core; open code uses loop saturators (`fasttanh`,
`tanh(0.2x)/0.2`). Digital stand-in for the transducer chain [derived]: input HPF
100–200 Hz, LPF 5.5–6.5 kHz (driver corner), pickup presence peak 2–4 kHz, soft clip
on the drive side; tank output above 7 kHz negligible. Fender 6G15: 12AT7 → DWELL →
RC HP ~300 Hz → 6K6 driver → tank → 12AX7 recovery.

## 8. Cortex-M7 notes

Budget 10,000 cycles/sample at 480 MHz / 48 kHz. FP MACs pipeline every cycle only
with ≥ 3 independent accumulations interleaved; result latency 3–5 cycles, so a
serial allpass chain alone runs ~5–8 cycles per section; interleave 2–3 springs to
approach 1 cycle/op. H743: ITCM 64 KB, DTCM 128 KB, AXI SRAM 512 KB; hot state in
DTCM, long lines in AXI through D-cache. DWT CYCCNT for measurement (unlock
`DWT->LAR = 0xC5ACCE55`).

## 9. Build plan (handover §9)

1. Harness with IR dump and host analysis (T_D, F_c, EDC).
2. ChowDSP-style single loop as sanity.
3. **Parametric spring, full rate**: C_lf one forward chain (M 100, K from f_C), DC
   blocker, chirp EQ, 10th-order elliptic as 5 SOS, loop delay, Gajarsky taps and
   modulation; C_hf M 189–200 K 1; mix. Validate T_D, F_c, decay, first-echo polarity.
4. Parker multirate.
5. Multi-spring (2–3), ÆLAPSE detune factors as a start, interleaved.
6. Drive/recovery and knock.
7. Optional modal bank.

Starting preset (Leem KA-1210 spring 1, DAFx-11 capped column, 44.1 kHz): T_D 56 ms,
F_c,lf 4275 (K 5.158), M_low 100, a_lf 0.63, g_lf −0.64, M_high 189, a_hf −0.34,
g_hf −0.83, chirp EQ 183/146/5 or 95/130, H_dc 40 Hz, H_low ellip(10, 1, 60, 4750),
loop delay T_D·fs − τ_DC, taps 1/0.1/0.1/0.01, mod pole 0.93 depth 8, C_hf delay
L/2.3, g_low 0.97, g_high 0.00097, c1 0.1. For 48 kHz scale sample quantities by
48/44.1 and recompute K and a2.

Validation: IR ≥ 3 s; first echo inverted; echo spacing = T_D (ACF lag); F_c by
per-bin ACF; EDC ≈ −5.4 dB per pulse; chain group-delay max at f_C,lf; 10-minute
float32 stability with white noise; DWT worst-case cycles per block.

## 10. Open questions

JAES 2010 internals (taps, modulation, a2 formula, EQ biquad, cross-couple gains);
feedback sign; Parker Table 1 fs/4 extremes to be verified numerically; no MCU
benchmark exists for any spring model; hexefx licence confirmed MIT in cubevox.

## 11. Sources

Papers: Parker & Bilbao DAFx-09; Bilbao & Parker TASLP 2010; Bilbao DAFx-13;
van Walstijn DAFx 2020; McQuillan & van Walstijn DAFx20in21; McQuillan et al. JAES
2025; Gamper/Parker/Välimäki DAFx-11; Parker EURASIP 2011; Pekonen et al. DAFx-09;
Välimäki et al. "Fifty Years of Artificial Reverberation" TASLP 2012; Parker thesis;
Abel & Smith DAFx-06. Patent US 8,391,504 B1. Code: Surge XT ChowDSP spring, BYOD,
chowdsp ModalSpringReverb, ÆLAPSE springs, hexefx, Faust reverbs.lib, Befaco,
Gajarsky and Gustafsson MATLAB, Parker JPverbRaw.dsp. Hardware: jnk0le M7 pipeline
tests, LLVM ARMScheduleM7.td, PJRC Teensy 4 docs, NXP RT1060 RM, STM32H743 memory
map. Tanks: ESP "Care and feeding of spring reverb tanks", amprepairparts, TAD.
