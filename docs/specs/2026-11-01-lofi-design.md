# Lo-fi card: the voice through a telephone, a radio or a broken walkie-talkie

Owner 2026-11-01: "also lo-fi voice spec". Offered for slot 7 in place of Gate, through the slot 7
row (`2026-11-01-slot-7-choices-design.md`). Spec only, not built.

## 1. Sound

1. TONE picks the kind of speaker: at the bottom a dark AM radio, at noon a telephone, at the top
   a thin megaphone.
2. AMOUNT runs from nearly clean through a narrow, pushed band to a crushed, aliased voice. The
   first half of the knob narrows the band and drives it; the second half adds the crush.
3. Slot 7 sits after Distortion and before the reverb, so the reverb hears the lo-fi voice, as a
   room would hear a speaker.

## 2. Panel

| Pot | Name | Kind | Readout |
|---|---|---|---|
| upper (knob A) | TONE | continuous, 0..100 % | `TONE 50 %` |
| lower (knob B) | AMOUNT | continuous, 0..100 % | `AMOUNT 50 %` |

1. Card id `lofi`, display name `Lo-fi`, Group None, Engine Lofi (new), cost row `costs::kLofi`.
2. Defaults: TONE 50 %, AMOUNT 50 %.

## 3. Engine (`engine/lofi.h`)

Signal path, in order:

1. Band. A 4th-order high-pass (two RBJ biquads, Q 0.54 and 1.31, Butterworth) and a 4th-order
   low-pass (the same pair). The band's centre `fc` follows TONE on a log scale, 600 Hz at 0 to
   2.4 kHz at 100 %. The corners close in with AMOUNT over its first half, `a = min(1, 2·AMOUNT)`:
   high-pass from 60 Hz to `fc / 2`, low-pass from 14 kHz to `2·fc`, each moving on a log scale in
   `a`. At TONE 50 % (`fc` 1.2 kHz) and AMOUNT 50 % the band is 600 Hz to 2.4 kHz, a telephone.
2. Honk. A peaking biquad at `fc`, Q 1.4, `+6·a` dB.
3. Drive. `tanhFast` (`engine/fastmath.h`) after a gain of `+18·a` dB, then the inverse of that
   gain's level rise from the makeup law (item 6).
4. Crush, AMOUNT's second half, `c = max(0, 2·AMOUNT − 1)`:
   - Sample and hold at a rate from 48 kHz down to 6 kHz on a log scale in `c`, with a fractional
     phase accumulator, so the rate moves without steps. No anti-alias filter: the aliasing is the
     sound.
   - Bit depth from 16 down to 5 bits in `c`: step `q = 2^(1 − bits)`, `y = q · round(x / q)`,
     round by an integer cast, no libm. Zero stays exactly zero, so silence stays silent.
5. The filter coefficients are set when a knob moves, once per block, from the 20 ms knob smoothers
   (`smooth.h`); libm runs only there.
6. Level. A makeup gain per AMOUNT position keeps program RMS within ±1 dB across the knob
   (measured on Adam's clips, stored as a short table over AMOUNT and interpolated), and `trimDb`
   in the tuning places the default setting at 0 to +0.5 dB. The band throws away most of the
   voice's energy below 500 Hz, so the makeup is large, up to about +12 dB.
7. Toggle off, settled: a plain copy, bit-exact (engine rule).

## 4. Firmware

1. `registry.cpp`: the card entry, `applyKnob`, `applyToggle`.
2. `chain_params.h`, `chain_run.h` and `.cpp`: `cv::LofiParams lofi`, `cv::Lofi lofi` in
   `ChainEngines`, the `Engine::Lofi` cases in `resetEngine` and `runStage`.
3. `costs.h`: `kLofi{2, 2, false}`, an estimate: five biquads, the tanh and the crush, about 80
   cycles a sample, 5k a block, 0.8 %, rounded up for the coefficient work of a block where a
   knob moves.

## 5. Checks

1. `lofi_test`: a sweep shows the band's −3 dB corners at the law's frequencies within 5 % for
   TONE 0, 50 and 100 % at AMOUNT 50 %; at AMOUNT 100 % the held rate and the bit depth match the
   law (count distinct held values over a ramp); silence in gives exact zeros out; settled off is
   a plain copy.
2. `knob_step_test`: TONE and AMOUNT steps and the toggle hold steady-state curvature.
3. `level_test`: the default setting lands 0 to +0.5 dB RMS on program; AMOUNT 0 to 100 % stays
   within ±1 dB.
4. `core_registry`: the card and its knobs.
5. Stress run with Lo-fi in slot 7 on the default and MOB + Chasm layouts.

## 6. CPU

| Layout | Mean % | Worst % |
|---|---|---|
| default, Lo-fi in slot 7 | 65 | 73 |
| MOB and Chasm, Lo-fi in slot 7 | 72 | 83 |
| lines | 75 | 95 |

## 7. Built 2026-11-01 (v2.00.95)

1. The crush runs ahead of the drive's inverse (item 3.3), so the 5-bit step lands on the driven
   signal: in the written order the step was five levels wide on the clips and 81 to 90 % of the
   samples were zero at AMOUNT 100 %; now 33 levels.
2. The makeup is a TONE by AMOUNT table (5 by 11, bilinear in dB), because TONE moved the level by
   ±4 to 6 dB and a sum of two one-knob tables could not hold. Every clip stays within 0.8 dB of
   unity at every position; the default reads +0.10, +0.37, −0.77 dB.
3. The filter coefficients ramp linearly across each block while a knob moves, so a step does not
   click (knob_step 1.003 and 1.011 against 1.05).
4. level_test's synthetic program reads −2.1 dB through the band; Lo-fi's level rows run on the clips.
