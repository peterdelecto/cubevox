# cubevox — SPRING B reverb (fourth engine in the reverb block)

Date 2026-10-03. Owner: "let's make a chasm-spring reverb but call it SPRING B …
keep the original chasm untouched." Goal: a spring sound at CHASM's cost, so it
fits the STM32H743 next to the rest of the chain. Parker Spring was off the face
(owner 2026-10-03) because it projected past the H7's budget (Parker Spring removed 2026-10-08).

## What it is

Three springs in parallel, each a feedback loop voiced from CHASM's parts (owner 2026-10-30:
more splash, chirp and liquid, less harsh; until then two loop halves fed each other). Code:
`engine/spring.h`, class `Spring`. It reuses `chasm_detail::Delay` and the one-pole from
`engine/dsp.h`.

Per spring: delay read (wobbled) × feedback g → one-pole treble loss → one-pole bass cut → two
diffusers → disperser → back into its own delay, plus input. Output = the three disperser outputs
summed → one-pole return low-pass × wetGain.

## Decisions

1. **Disperser inside the loop.** `chirpSections` stretched allpasses
   `(a + z^-K) / (1 + a z^-K)`, K = 6 (sweep below fs/2K = 4 kHz), |a| = 0.80, 32 sections per
   spring. `chirpSign` +1 (default): lows arrive first, each echo sweeps up, as the Abel and
   Parker spring measurements give; −1 sweeps down. The owner picked up by ear on 2026-10-30
   from renders of both. Inside the loop, echo n carries n passes, so the sweep grows each
   repeat.
2. **Three springs of different lengths.** `springMs1 / 2 / 3` = 37 / 45 / 56 ms, tap to tap,
   diffuser delay included, with no short common multiple, so the three chirp trains stack into
   splash. A ring of three halves was tried and rejected: with the input entering every line,
   all three paths share one 138 ms lap and the echoes land together once a lap, a boing at
   7 Hz.
3. **Light diffusion.** Diffuser coefficient 0.30 (CHASM 0.6), lengths 2.9 / 4.3, 3.7 / 5.3 and
   3.4 / 4.9 ms, so repeats stay distinct. Wobble LFO phases 0, 85 and 171 of 256.
4. **Level across DECAY.** Input × sqrt((1 − g²) / g), feedback floor 0.10, so the tail's RMS holds
   as feedback moves (measured +0.40 dB from DECAY 0 to 1).
5. **Feedback** `timeLo / timeHi` = 0.55 / 0.86 per spring, ceiling 0.97. TIME curve
   `kSpringDecay` {0.25, 0.50, 0.85}. T60 at TIME 0 / 50 / 100 %: 0.71 / 0.89 / 1.36 s.
6. **Dwell** is a tuning value with no knob (0.20). It drives CHASM's soft clip
   (`kChasmClipHeadroom`), drive 4× (was 8×, which pushed chirp into clang), comp 0.8.
7. **Damping.** In-loop treble loss 9 kHz (was 6) and bass cut 200 Hz keep the flutter alive in
   the tail; the harsh top goes at the return instead, `returnLpHz` = 5 kHz one-pole on the summed
   output. Measured on the wet impulse: 10 to 20 kHz 3.7 dB lower than before, the 1 to 6 kHz
   ringing peak 2.4 dB lower, echo density at 100 ms 0.93 (was 0.83).
8. **wetDb** = +6.0 dB, trimmed by `level_test` on the wet alone at the box's
   TIME 50 % and dwell 0.20.

## Panel and prototype

1. Panel: MIX + TIME (`2026-10-07-v2-mic-reverb-time-slapback.md`), as every reverb card. DWELL stays
   a tuning field off the face. The engine is Spring (`engine/spring.h`, the SPRING B design) and its
   card is named "Spring Reverb".
2. Tuning macros: Drip (chirpA 0.55/0.70/0.85, sections 12/24/32), Flutter
   (wobble 2/8/24 samples at 1.5/3/5 Hz, diffusion 0.45/0.30/0.15), Brightness
   (treble loss 3/6/10 kHz). All default 50 %.
3. Advanced: raw sliders under "Spring" and "Spring levels".
4. Render: `--reverb spring` (`springb` also accepted), tuning keys `sb*`.

## Cost

| | Value |
|---|---|
| Allpasses / sample | 6 diffusers + 96 disperser sections, interleaved two per pass (CHASM: 8 + 64) |
| Mac render, 5 s clip, 2026-10-30 | 1.06× the two-half engine (103.6 → 109.6 ms, median of five) |
| H7, instruction count estimate | 1.30× the two-half engine's 72k worst cycles; the measured figure goes in `costs.h` after the bench |

## Tests

`spring_test`: the three echo spacings, the sweep that grows from echo 1 to 2 for both signs,
level across DECAY within 1 dB, stability at full DECAY and dwell 1 over 20 s, the return
low-pass corner, Reverb on=false bit-exact and no allocation. `level_test`, `knob_step_test`
and `reverb_clean_test` carry it.
