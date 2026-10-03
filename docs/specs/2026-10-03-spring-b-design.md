# cubevox — SPRING B reverb (fourth engine in the reverb block)

Date 2026-10-03. Owner: "let's make a chasm-spring reverb but call it SPRING B …
keep the original chasm untouched." Goal: a spring sound at CHASM's cost, so it
fits the STM32H743 next to the rest of the chain. PARKER SPRING is off the face
(owner 2026-10-03) because it projects past the H7's budget.

## What it is

CHASM's two-half feedback loop, voiced as a spring. Code: `engine/spring_b.h`,
class `SpringB`. It reuses `chasm_detail::Delay` and `onePole`; `chasm.h` is unchanged.

Per half: delay read (wobbled) × feedback g → one-pole treble loss → one-pole
bass cut → two diffusers → disperser → write into the other half's delay, plus
input. Output = the two disperser outputs summed × wetGain.

## Decisions

1. **Disperser inside the loop.** `chirpSections` stretched allpasses
   `(a + z^-K) / (1 + a z^-K)`, K = 6 (sweep below fs/2K = 4 kHz), a = +0.70.
   Positive a: lows arrive first, each echo sweeps up, as in the SPRING model.
   Inside the loop, echo n carries n passes, so the sweep grows each repeat.
   CHASM's output chirp (falling) is left out.
2. **Short echo spacing.** `halfMs1 / halfMs2` = 33 / 41 ms, tap to tap,
   diffuser delay included. CHASM's lap is ~170 ms.
3. **Light diffusion.** Diffuser coefficient 0.30 (CHASM 0.6), lengths
   2.9 / 4.3 ms and 3.7 / 5.3 ms, so repeats stay distinct.
4. **Level across DECAY.** Input × sqrt(1 − g²), so the tail's RMS holds as
   feedback moves (measured −0.75 dB from DECAY 0 to 1).
5. **Feedback** `timeLo / timeHi` = 0.70 / 0.93 per half, ceiling 0.97.
   Intensity curve `kSpringBDecay` {0.25, 0.50, 0.85}.
6. **DWELL** reuses CHASM's soft clip (`kChasmClipHeadroom`), drive 8×, comp 0.8.
7. **wetDb** = +7.9 dB, trimmed by `level_test` (wet only at MIX 1: +0.24 dB).

## Panel and prototype

1. Panel: INTENSITY + DWELL, as every reverb engine. Menu name "SPRING B".
2. Tuning macros: Drip (chirpA 0.55/0.70/0.85, sections 12/24/32), Flutter
   (wobble 2/8/24 samples at 1.5/3/5 Hz, diffusion 0.45/0.30/0.15), Brightness
   (treble loss 3/6/10 kHz). All default 50 %.
3. Advanced: raw sliders under "Spring B" and "Spring B levels".
4. Render: `--reverb springb`, tuning keys `sb*`.

## Cost

| | Value |
|---|---|
| Allpasses / sample | 4 diffusers + 48 disperser sections (CHASM: 8 + 64) |
| State | 36.5 KB |
| Full chain, M2 no-SIMD, 34.3 s loop | 0.68 s (CHASM 0.87 s, SPRING 1.80 s) |
| Projected H7 full chain | ~20–40 % (estimate, ×10–20 scaling) |

## Tests

`spring_b_test`: echo spacing, upward sweep that doubles from echo 1 to 2,
level across DECAY, stability at full DECAY + DWELL, Reverb on=false bit-exact
and no allocation. `level_test` and `reverb_clean_test` carry SPRING B.
