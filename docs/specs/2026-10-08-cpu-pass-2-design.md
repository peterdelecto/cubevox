# cubevox — CPU pass 2 (Unison, Distortion, FFT coarse pitch, budget rule)

Date 2026-10-08. Owner: do both A (Unison and Distortion passes) and B (FFT coarse
pass in the pitch tracker), then the measured cost table and the 75 % rule. Prudent and
proper; rework is never a cost. Nothing here is started until the owner says build.
Follows `2026-10-08-pitch-cpu-pass-design.md`; its gates, bench procedure and commit
rules carry over. Budget per 64-sample block is 640,000 cycles (100 %).

## Where it stands (e06d829, voice clip, all cards on, WeAct -O2)

| Slot | Avg | Worst | Off, settled |
|---|---|---|---|
| Pitch (Autotune + Octave) | 21.9 % | 45.0 % | 0.9 % |
| Unison | 20.7 % | 21.3 % | 15.9 % |
| Distortion | 13.9 % | 23.4 % | ~0 (skips at drive 0) |
| Chain | 80.5 % | 113.6 % | 27 % |

Every hop block (one in four) is late. The hop block carries the tracker burst of
about 31 % on top of the chain average.

## Where the cycles go

Estimates from the code against the measured slot totals.

### Unison (`engine/unison.h`), 2062 cycles per sample on, 1590 off

| Per sample | Count | Note |
|---|---|---|
| `sinf` LFO | 2 | one per voice, 0.2..0.9 Hz |
| `cosf` Hann crossfade | 4 | two taps per voice; default detune is 0 so tap 0 has gain 0 and is skipped after its `cosf` |
| `powf` | 3 | pitch ratio per voice (depends on `depth_`), output trim (depends on `depth_`) |
| `floorf` + `%` | 6 | `frac()` per tap, `readCubic` index, `writePos_` |
| Catmull-Rom reads | 2..4 | the real work, about 30 cycles each |

Off and settled (`depth_` 0) runs the full loop with `wetGain` 0 and trim 1, so
the stage costs 102k doing nothing.

### Distortion (`engine/distortion.h`), 1386 cycles per sample avg, 2340 worst

| Per sample | Count | Note |
|---|---|---|
| `expf` stage gains | 2 | depend on `drive_` only |
| `setTrebleShelf` | 0 or 1 | runs every sample while `tone_` is still gliding: `powf`, `cosf`, `sinf`, `sqrtf`, two divides. This is the 150k worst block |
| `makeup()` | 1 | `sqrtf` and two divides, depends on `drive_` only |
| `tanhf` | 0..4 | rail soft edge, signal dependent, both saturators at 2x |
| history shifts | 24 moves | `shiftIn` ×2 and the odd-phase shift, per saturator |
| filters | 5 poles + 5 biquads | ~40 flops, stays |

Per block, eleven filter `set` calls recompute their coefficients from constant
tuning (about 3k per block).

### Tracker coarse pass (`engine/pitch.h`), about 170k on the hop block

171 lags × 256 MACs = 43.8k MACs at ~3 cycles each, instruction-issue bound. The
fine pass is 9 × 1024 = 9.2k MACs (~30k) and stays as it is.

## Decisions

1. Order: Unison, then Distortion, then the FFT coarse pass, then the cost table and
   rule. Each stage is its own commits; a stage that fails its gate is redone, the
   gate is never loosened.
2. Smoothers keep stepping per sample on their current path (`depth_`, `drive_`,
   `tone_`), so snap points and settled values are unchanged. Every quantity that is
   a function of a smoother alone (Unison ratio and trim, Distortion stage gains and
   makeup, treble shelf) is evaluated at the block edges and interpolated linearly
   across the block, the pattern from pitch pass step 4. Exact once settled, so the
   level rule and every bypass test hold bit-exact.
3. Unison LFO and shifter sawtooth become 32-bit integer phase accumulators
   (`uint32_t`, one turn = 2^32). Increment per block from `lfoHz` (the Rate knob
   changes it live) and from `(1 - ratio) / windowSmp`. Integer phase has no drift
   over hours and no `frac()`; the float accumulator it replaces rounds at ~1e-7
   per step. Sine comes from a new `sinTurns(uint32_t)` in `engine/fastmath.h`
   (odd polynomial on a quarter turn, absolute error under 1e-6, tested). Hann
   gains are `g0 = sin²(half turn)`, `g1 = 1 - g0`, so they sum to exactly 1.
4. Unison with the shifter parked (ratio exactly 1, phase 0, the default tuning)
   reads one tap per voice and skips the window. The crossfaded path stays live for
   any nonzero `detuneCents`.
5. Unison off and settled: write the delay line, copy in to out, advance the LFO
   phases by `n * inc` (one integer add each). Target under 3k per block.
6. Distortion `tanhf` becomes a rational approximation in `engine/fastmath.h`
   (`tanhFast`, odd rational of the Eigen form, argument clamped to ±9, absolute error
   under 1e-5, tested). The soft-edge shape and the 2x oversampling are unchanged;
   the owner listens to the Distortion A/B render before the commit.
7. Distortion filter coefficients recompute only when their inputs change: tuning
   values are compared to the last applied copy, the treble shelf is set once per
   block from the block-start `tone_`. The worst block then equals the average block.
8. Oversampler histories become ring-indexed (no shifts) only if a measurement after
   decisions 6 and 7 shows the shifts above 3k per block; otherwise left alone.
9. Coarse YIN by FFT. The difference function expands as
   d(τ) = E(0) + E(τ) − 2 r(τ), with E(τ) = Σ_{j<W} x[j+τ]² from one prefix sum over
   the frame and r(τ) = Σ_{j<W} x[j] x[j+τ] the cross-correlation of the window
   (x[0..255]) with the frame (x[0..426]). Both zero-padded to N = 512: for τ ≤ 171 and
   j ≤ 255, j + τ ≤ 426 < 512, so the circular correlation is exactly linear.
   r = IFFT(conj(FFT(window)) · FFT(frame)). Three real 512-point transforms plus 257
   complex products, about 40k flops against 131k today; expected 60..80k cycles
   for the coarse pass, hop block ~200k → ~110k. d′ and the pick rule are unchanged.
   Cancellation in E − 2r near the minimum is ~1e-6 of E, far below the 0.15
   threshold and the ±1-lag precision the fine pass needs from the coarse pick.
10. The FFT is a new portable `engine/fft.h`: fixed N = 512 real transform via a
    256-point complex radix-2 iterative FFT, twiddles in a `std::array` built once at
    static init, no heap, float only, `-Werror` clean, included unchanged by the H7.
    CMSIS `arm_rfft_fast_f32` is considered only on evidence, if the own FFT measures
    over 1.5x its published cycle count on the M7.
11. Budget rule becomes two sums over the placed cards: average ≤ 75 % and worst
    block ≤ 95 %. `core/costs.h` carries both numbers per card from the bench,
    `measured = true` for every card the WeAct has measured. The app shows the
    average sum; placement refuses a layout that breaks either sum.

## Steps

Each step: edit, `cmake --build build -j && ctest --test-dir build`, `tools/engine_ab.sh
<previous step commit> --tol <dB>`, WeAct bench (`./term.py -s 0vm -s p -s p -w 12`,
read avg, worst, per slot, slot worst), commit with the numbers. Reference for the
first step is e06d829 (b2327c2 is docs only).

| Step | Change | Gate | Expected |
|---|---|---|---|
| U1 | Unison: block-edge ratio and trim, integer phase LFO with `sinTurns`, integer sawtooth with exact-sum Hann gains, parked-shifter single tap, compare wraps, idle path | new `unison_test` case: per-voice delay from a test hook within 0.05 samples of the analytic sine over 10 s at Rate 0, 50, 100 %; `sinTurns` error test; `level_test` rows exact; A/B unison and chain within -40 dB (the old float phase random-walks ~1e-4 rad over the clip, so the two LFOs differ slightly in phase; the analytic test is the real guard); owner listens to the Unison A/B pair | 132.6k → under 30k on, 102k → under 3k off |
| D1 | Distortion: block-edge `g1`, `g2`, `trim/makeup`; coefficient sets only on change; shelf once per block | A/B distortion within -60 dB (differs only during the 20 ms drive ramp at the render head), bypass exact, `distortion_test` | 88.7k → ~55k avg; worst 150k → within 10 % of avg |
| D2 | `tanhFast` in the rail | `fastmath_test` tanh error under 1e-5 over ±9 and exactly ±1 beyond; A/B distortion within -60 dB; owner listens | ~55k → under 35k |
| D3 | Oversampler ring histories, only if D2 measurement says shifts exceed 3k | bit-exact | — |
| F1 | `engine/fft.h` + `test/fft_test.cpp`; nothing wired in | forward vs naive DFT on random input, relative RMS error under 1e-6; round trip under 1e-6; impulse and DC cases exact to 1e-7; no allocation | host only |
| F2 | `PitchTracker::coarse()` by FFT, `sumSqDiff2` removed, `cmnd_` filled from d(τ) | `pitch` test (vibrato 3 cents, steady, silence); new `pitch_test` case through a test hook that runs the direct CMND on the same frames: coarse pick within ±1 lag on ≥ 99.5 % of hops and the same voicing decision on ≥ 99 % of hops over the vibrato tone and the voice clip; hard tune ≤ 4 cents; A/B autotune, octave, chain within -60 dB | hop block ~200k → ~110k; pitch slot worst 288k → ~200k, avg 140k → ~115k |
| C1 | `core/costs.h` two columns from the bench, `measured = true`, rule 75 % / 95 %, `registry.cpp`, `test/firmware_test.cpp`, Mac app placement check, `2026-10-08-effect-registry-design.md` | `ctest` (firmware test checks the default layout against both sums); WeAct bench of the default layout: no late blocks on the voice clip | chain ~52 % avg, ~67 % worst (estimate) |
| C2 | This spec's "where it stands" table filled; CLAUDE.md spec list | — | — |

## Tests

1. `unison_test`: analytic LFO delay check through a `delayAt(v)` test hook, at three
   Rate settings and Depth 0.8, 10 s. Existing rows stay.
2. `fastmath_test`: `sinTurns` absolute error under 1e-6 on 1e5 phases; `tanhFast`
   absolute error under 1e-5 on ±9, exact ±1 past the clamp, odd symmetry exact.
3. `fft_test`: as F1.
4. `pitch_test`: as F2, direct CMND oracle lives in the test, not the engine.
5. `firmware_test`: default layout within both sums; every card `measured`.
6. Existing gates unchanged: hard tune ≤ 4 cents, bypass exactness, `level_test`.

## Estimates after all steps (voice clip, all cards on)

| | Now | Expected |
|---|---|---|
| Chain average | 80.5 % | ~52 % |
| Chain worst block | 113.6 % | ~67 % |
| Pitch slot worst | 45.0 % | ~31 % |
| Unison | 20.7 % | ~4 % |
| Distortion | 13.9 % | ~5 % |

These are code-reading estimates; the bench decides. A step that misses its expected
number by more than a third is examined before the next step starts.

## Ruled out

1. Spreading the YIN analysis across blocks (hard-tune gate, pitch pass decision 1).
2. Sliding incremental YIN (float drift after loud passages needs a periodic rebase).
3. Coarser decimation for the coarse pass (8 kHz): saves a third, needs a wider fine
   reach, and the FFT saves more.
4. Float phase accumulators with a rotor for the Unison LFO: at 0.2 Hz `cosf(inc)`
   rounds to 1.0f and the coupled form drifts in amplitude; the integer accumulator
   has neither problem.
5. Lowering the hard-tune gate or any A/B tolerance to make a step pass.
6. Dropping the Distortion 2x oversampling.

## Done when

`ctest` passes with the new cases; every A/B case is within its stated tolerance; the
owner has listened to the Unison and Distortion pairs; the WeAct runs the default layout
on the voice clip with no late blocks and both sums under their lines; `costs.h` holds
measured numbers for every card.
