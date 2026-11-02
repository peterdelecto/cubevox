# cubevox — Spring reverb (one of the two engines in the reverb block)

Date 2026-10-03. Owner: "let's make a chasm-spring reverb but call it SPRING B …
keep the original chasm untouched." Goal: a spring sound at CHASM's cost, so it
fits the STM32H743 next to the rest of the chain. Parker Spring was off the face
(owner 2026-10-03) because it projected past the H7's budget (Parker Spring removed 2026-10-08).
The reverb block holds two engines, Spring and Chasm. The card is "Spring Reverb".

## What it is

Three springs in parallel, each a feedback loop voiced from CHASM's parts (owner 2026-10-30:
more splash, chirp and liquid, less harsh; until then two loop halves fed each other). Code:
`engine/spring.h`, class `Spring`. It shares `Allpass`, `Delay`, the LFO phase step and the
sine table with Chasm through `engine/reverb_parts.h`, and the one-pole from `engine/dsp.h`.

Per spring: delay read (wobbled) × feedback g → one-pole treble loss → one-pole bass cut → two
diffusers → disperser → back into its own delay, plus input. wetDb is the one gain ahead of the
loops; the loops are linear, so it scales the wet. Output = the three disperser outputs summed →
one-pole return low-pass. The reverb wet has no DC blocker (section 9).

The loops run at 24 kHz (owner pick C, 2026-11-02). A 15-tap Kaiser (β 3.0) half-band decimator
feeds them and a matching interpolator brings the wet back to 48 kHz. Each spring's delay length
subtracts the two filters' delay (`kFilterDelay`, 8 samples, kept whole so delay reads stay on
whole samples), so the echoes land where they did: 36.98 / 44.98 / 55.98 ms. The output is
bit-exact for any call size from 1 to 1,000 samples. The half-band pair passes echo 1 at −1.5 dB
at 8 kHz, −3.2 dB at 10 kHz, −8 dB at 12 kHz and −48 dB at 16 kHz against 1 kHz.

## Decisions

1. **Disperser inside the loop.** `chirpSections` stretched allpasses
   `(a + z^-K) / (1 + a z^-K)`, K = 3 at the 24 kHz loop rate (sweep below fs/2K = 4 kHz),
   |a| = 0.846, 24 sections per spring (the 48 kHz loops took 32 at 0.80). Lows arrive first and each echo sweeps up, as the Abel and Parker spring measurements
   give. The owner picked up by ear on 2026-10-30 from renders of both directions. Inside the loop,
   echo n carries n passes, so the sweep grows each repeat.
2. **Three springs of different lengths.** `springMs1 / 2 / 3` = 37 / 45 / 56 ms, tap to tap,
   diffuser delay included, with no short common multiple, so the three chirp trains stack into
   splash. A ring of three halves was tried and rejected: with the input entering every line,
   all three paths share one 138 ms lap and the echoes land together once a lap, a boing at
   7 Hz. Each line holds its spring's default read plus 16 samples: 1,446 / 1,744 / 2,308 floats.
3. **Light diffusion.** Diffuser coefficient 0.30 (CHASM 0.6), lengths 2.9 / 4.3, 3.7 / 5.3 and
   3.4 / 4.9 ms, so repeats stay distinct. Wobble LFO phases 0, 85 and 171 of 256.
4. **Level across DECAY.** Input × sqrt((1 − g²) / g), feedback floor 0.10, so the tail's RMS holds
   as feedback moves (measured +0.40 dB from DECAY 0 to 1).
5. **Feedback** `feedbackLo / feedbackHi` = 0.55 / 0.86 per spring, ceiling 0.97. TIME curve
   `kSpringDecay` {0.25, 0.50, 0.85}. T60 at TIME 0 / 50 / 100 %: 0.73 / 0.94 / 1.41 s
   (Schroeder decay from −5 to −35 dB, doubled), measured on the 48 kHz loops; the half-rate
   loops keep the feedback curve.
6. **Dwell** is Spring's own tuning value with no knob, `SpringTuning::dwell` = 0.20. It drives the
   reverb soft clip (`kReverbClipHeadroom`), drive 4× (was 8×, which pushed chirp into clang),
   comp 0.8.
7. **Damping.** In-loop treble loss 9 kHz (was 6) and bass cut 200 Hz keep the flutter alive in
   the tail; the harsh top goes at the return instead, `returnLpHz` = 5 kHz one-pole on the summed
   output. Measured on the wet impulse: 10 to 20 kHz 3.7 dB lower than before, the 1 to 6 kHz
   ringing peak 2.4 dB lower, echo density at 100 ms 0.93 (was 0.83).
8. **wetDb** = +0.37 dB at the loop input, set by `level_test` on the wet alone at the box's
   TIME 50 % and dwell 0.20, judged on the mean of Adam's three clips (owner decision 2026-11-01:
   Adam's clips govern the reverb level). The clip rows read +0.92 / +0.23 / −0.54 dB, mean
   +0.20 dB. TIME rows at 0 / 50 / 100 %: +0.01 / +0.20 / +0.07 dB. MIX 50 % rows sit within
   ±0.15 dB.
9. **No DC blocker** (owner 2026-11-01: "we can remove the dc blocker if it does nothing"). A 0.5
   DC step leaves the wet mean at −129 to −145 dBFS on Spring and −141 to −164 dBFS on Chasm
   (TIME 0 / 50 / 100 %). `reverb_clean_test` holds the wet mean under −90 dBFS.
10. **Rest.** With a silent input the springs run on until every loop sits below `restLevel`
    (1e-6, −120 dBFS; 0 never rests), then stop computing. The first block with sound wakes them
    from the state they rested in. At decay 1, the top of the engine's range, they rest 3.56 s
    after the sound stops; a resting run stays within 1.6e-6 (−116 dBFS) of one that never rests. Spring keeps running at MIX 0, so a tail is
    there when MIX comes back up.
11. **Wobble rate** clamps at 10 Hz. A `static_assert` holds the wobble's read error at the block
    edges under 0.05 samples at the clamp; 20 Hz read 0.057. The sine is worked out at the block
    edges of the 24 kHz loops and read linearly between.

## Panel and prototype

1. Panel: MIX + TIME (`2026-10-07-v2-mic-reverb-time-slapback.md`), as every reverb card. Dwell stays
   a tuning field off the face. The engine is Spring (`engine/spring.h`) and its card is named
   "Spring Reverb".
2. Tuning macros: Chirp (chirpA 0.65/0.846/0.92, sections 16/24/32), Flutter
   (wobble 2/8/24 samples at 1.5/3/5 Hz, diffusion 0.45/0.30/0.15), Brightness
   (treble loss 4.5/9/14 kHz). All default 50 %.
3. Advanced: raw sliders under "Spring" and "Spring levels".
4. Render: `--reverb spring`, `--time` and `--mix` (the panel positions), tuning keys `spring*`
   (`springDwell`, `springWetDb`, `springRestLevel` and the rest); a key outside its range is
   refused.

## Cost

| | Value |
|---|---|
| Allpasses per 24 kHz sample | 6 diffusers + 72 disperser sections (Chasm: 4 loop allpasses + 32 chirp units at 48 kHz) |
| Board, 2026-11-01, the 48 kHz engine | 83.4k mean / 84.0k worst cycles (13.0 % / 13.1 %), slot 8, knobs at top; the swept stress run of the same day read 82.9k / 88.6k (13.0 % / 13.8 %), `docs/audits/2026-11-01-stress-run.md` |
| Host, ns a sample, knobs at top / MIX moving | clang 45.6 → 20.5 / 56.2 → 31.1; g++ 42.5 → 18.1 / 52.8 → 27.9 |
| M7 model (llvm-mca, firmware GCC) | 35.3k cycles a block (5.5 %), 39.3k with MIX moving; the model read the 48 kHz engine 13 % over its board figure |
| `costs.h` | `kReverbSpring {6, 7, false}`, from the M7 model until a stress run measures the engine |
| RAM | `sizeof(Spring)` 14,844 B |

## Tests

`spring_test` (14 checks): the three echo spacings, the sweep that grows from echo 1 to 2, the
half-band response at 8, 10 and 12 kHz, bit-exact output at every call size, level across TIME,
stability at the shipped tuning and at every clamp ceiling, the disperser against a
section-by-section reference, the return low-pass corner read at 8 kHz, Reverb settled off bit-exact and no
allocation, restart clear after an engine switch or toggle, tuning values far out of range staying
finite, the rest and its wake, and the RAM bound. `level_test` (Adam's clips), `knob_step_test`
(both knobs also move their whole travel in one step) and `reverb_clean_test` (the dwell clipper and
the wet DC) carry it.
