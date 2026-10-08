# cubevox — CPU pass 2 (Unison, Distortion, pane tracker, PSOLA weights, budget rule)

Date 2026-10-08. Owner: do both A (Unison and Distortion passes) and B (the tracker
burst), then the measured cost table and the 75 % rule. Prudent and proper; rework is
never a cost. Nothing here is started until the owner says build.
Follows `2026-10-08-pitch-cpu-pass-design.md`; its gates, bench procedure and commit
rules carry over. The tracker plan takes its shape from the research note
`docs/audits/2026-10-08-pitch-tracker-research.md` (panes, predicted-lag fine pass,
constant-delay PSOLA weights, placement). Budget per 64-sample block is 640,000
cycles (100 %).

## Where it stands (e06d829, voice clip, all cards on, WeAct -O2)

| Slot | Avg | Worst | Off, settled |
|---|---|---|---|
| Pitch (Autotune + Octave) | 21.9 % | 45.0 % | 0.9 % |
| Unison | 20.7 % | 21.3 % | 15.9 % |
| Distortion | 13.9 % | 23.4 % | ~0 (skips at drive 0) |
| Chain | 80.5 % | 113.6 % | 27 % |

Every hop block (one in four) is late. The hop block carries the whole tracker
analysis, about 200k cycles, on top of the pitch slot's per-block base of about 90k
(PSOLA voice ~36k, shared loop ~23k, front end). 288k measured worst against that
~290k sum, so the hop block is accounted for by work, not by cache state; placement
is still tried first because it is free and bit-identical.

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

### Tracker (`engine/pitch.h`), about 200k on the hop block, 0 elsewhere

Coarse 171 lags × 256 = 43.8k MACs (~131k cycles at the measured ~3 cycles per MAC;
VSUB + VFMA is two FP issue slots, so ~2 is the floor for the difference form), fine
9 × 1024 = 9.2k MACs (~30k), CMND, pick, frame RMS and the median (~10k), plus
memory effects. All of it runs once per hop (256 samples, 4 blocks) at the hop
boundary, which is why one block in four carries it.

### PSOLA voice (`engine/psola.h`), ~36k per block per voice

Per grain-sample: `readCubic` does a `floorf`, a float-to-int, three wraps and a
cubic in Horner form (~20 ops), then the window rotor (~6) and bookkeeping. Each
grain reads at a constant delay, so the fraction never changes over its life.

## Decisions

1. Order: Unison, Distortion, placement, PSOLA weights, coarse panes, cost table and
   rule. The fine-pass panes (decision 11) are held behind a stated trigger. Each
   stage is its own commits; a stage that fails its gate is redone, the gate is never
   loosened.
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
9. Placement. The STM32duino H743 linker script puts `.data` and `.bss` in AXI SRAM
   (0x24000000, behind the 16 KB D-cache, which the core enables in `main.cpp`).
   The firmware already links `firmware/platform/h7_h743v_sections.ld`, which adds a
   NOLOAD `.dtcm_bss` section at 0x20000000 with an overflow assert, and
   `h7_block_mem.h` provides the `H7_DTCM_BSS` attribute and `h7_dtcm_bss_zero()`.
   The pitch stage object (tracker rings, pane array, voice ring, LP rings, 101,736 B
   measured) leaves the `Chain` struct and is defined with that attribute in
   `board/audio.cpp`. Because the section is NOLOAD and static construction runs
   before `audioInit()` zeroes it, `audioInit()` constructs the object again in place
   (placement new); `reset()` alone would leave the tracker's const anti-alias biquad
   zeroed and Autotune silently unvoiced. Engine headers are untouched. Bit-identical;
   measured before any tracker change so the pane step is compared against a
   placement-clean baseline.
10. Coarse YIN by panes. The coarse window is W = 256 decimated samples = 16 blocks
    and the hop is 4 blocks, so d(τ) splits into 16 block-aligned partial sums.
    Pane P_s(τ) = Σ_{j ∈ pane s} (x[j] − x[j+τ])² over the 16 j-values whose newest
    needed sample x[j+171] is the newest sample when pane s is computed; the pane's
    j-range is the 16 oldest of the newest 187 samples. One pane per 16 decimated
    samples (counter-triggered inside `push`, so any caller block size works),
    171 lags × 16 = 2,736 MACs with the paired-lag kernel, ~8k cycles every block.
    At the hop boundary d(τ) = Σ of the last 16 panes (2,736 adds), then the CMND,
    pick and parabola exactly as today. Same window, same statistic, same alignment,
    no added latency: every pane uses only samples that have arrived. Summation
    order differs (tree height ~31 against ~67 today), so the step is -60 dB class,
    not bit-exact, and has no drift because nothing is ever subtracted. Storage
    16 × 172 floats, 11 KB, in the tracker object. The coarse burst of ~131k leaves
    the hop block; what remains there is the fine pass, CMND/pick, frame RMS (~40k).
    `static_assert`s pin the geometry: `kBlock % kDecim == 0`,
    `kCoarseW % (kBlock / kDecim) == 0`, `kHop % kBlock == 0`.
11. Fine pass by predicted-lag panes, held. Same decomposition at 48 kHz (16 panes of
    64 j-values over W = 1024) for a lag set predicted from the last published period,
    ±12 lags (25 × 64 = 1,600 MACs per block, ~5k cycles). Each pane records its lag
    base; at the hop, if [T−4, T+4] lies inside every one of the 16 panes' sets, the
    nine d values are summed from the panes, else today's direct 9-lag search runs as
    the fallback (9.2k MACs, hop block only) and the fallback count is reported in the
    bench. Vibrato at 5 Hz ±30 cents moves the period ~0.3 % per hop, well inside the
    margin, so fallbacks happen at onsets and octave corrections. Trigger to build it:
    after steps U1, D2 and F1 the pitch slot's worst block still sets the chain's
    worst block by more than 5 % of the budget, or a layout the owner wants (two pitch
    cards, Chasm) fails the 95 % worst-block sum. Until then the ~30k fine burst stays.
12. PSOLA Catmull-Rom weights computed once at grain launch. The grain's delay is
    constant, so the read fraction f is constant and the cubic is a fixed 4-tap FIR:
    h₋₁ = −½f + f² − ½f³, h₀ = 1 − 2.5f² + 1.5f³, h₁ = ½f + 2f² − 1.5f³,
    h₂ = −½f² + ½f³, y = Σ h·x. Per grain-sample 4 loads, 4 multiplies, 3 adds and
    one integer index step instead of ~20 ops with a `floorf`. Not bit-exact
    (different association), -60 dB class, expected far below. Grain pool 8 → 6:
    at r ≤ 2 live grains are at most ⌈2r⌉ + 1 = 5. Applies to `PsolaVoice` only;
    the Unison line and the granular shifters move their read points per sample.
13. Budget rule becomes two sums over the placed cards: average ≤ 75 % and worst
    block ≤ 95 %. `core/costs.h` carries both numbers per card from the bench,
    `measured = true` for every card the WeAct has measured. The app shows the
    average sum; placement refuses a layout that breaks either sum.

## Steps

Each step: edit, `cmake --build build -j && ctest --test-dir build`, `tools/engine_ab.sh
<previous step commit> --tol <dB>`, WeAct bench (`./term.py -s 0vm -s p -s p -w 12`,
read avg, worst, per slot, slot worst), commit with the numbers. Reference for the
first step is e06d829 (b2327c2 and 5d76b06 are docs only). Always `cd` with absolute
paths; parallel calls inherit a drifting cwd.

| Step | Change | Gate | Expected |
|---|---|---|---|
| U1 | Unison: block-edge ratio and trim, integer phase LFO with `sinTurns`, integer sawtooth with exact-sum Hann gains, parked-shifter single tap, compare wraps, idle path | new `unison_test` case: per-voice delay from a test hook within 0.05 samples of the analytic sine over 10 s at Rate 0, 50, 100 %; `sinTurns` error test; `level_test` rows exact; A/B unison and chain within -40 dB (the old float phase random-walks ~1e-4 rad over the clip, so the two LFOs differ slightly in phase; the analytic test is the real guard); owner listens to the Unison A/B pair. Measured: unison -39.7 dB, chain -38.4 dB, all other cases exact. The miss is the old code's: against a copy of e06d829 with a double-precision phase, the old float Unison sits at -31.8 dB and the new one at -66.4 dB on a 4 s synthetic clip, so the A/B reference is the less accurate side. Analytic LFO test 3.7e-5 samples, `sinTurns` 1.6e-7. Listening pair in `build/listen/unison_{old_e06d829,new_u1}.wav` (depth 0.5) | 132.6k → under 30k on, 102k → under 3k off. Bench pending: the WeAct was off the bus |
| D1 | Distortion: block-edge `g1`, `g2`, `trim/makeup`; coefficient sets only on change; shelf once per block | A/B distortion within -60 dB (differs only during the 20 ms drive ramp at the render head), bypass exact, `distortion_test` | 88.7k → ~55k avg; worst 150k → within 10 % of avg |
| D2 | `tanhFast` in the rail | `fastmath_test` tanh error under 1e-5 over ±9 and exactly ±1 beyond; A/B distortion within -60 dB; owner listens | ~55k → under 35k |
| D3 | Oversampler ring histories, only if D2 measurement says shifts exceed 3k | bit-exact | — |
| M1 | `firmware/variant/` with a `.dtcm` section, pitch stage object placed there, D-cache state confirmed in the boot log | bit-identical (A/B not needed: engine untouched); firmware builds warning-free; bench | unknown; if the hop block drops, the drop is memory, and the pane baseline is taken after this step |
| P1 | PSOLA launch-time weights, pool 6, integer index step | A/B autotune, octave, chain within -60 dB (expected under -90); hard tune ≤ 4 cents; octave bypass exact | PSOLA voice ~36k → ~22k; pitch slot avg −14k |
| F1 | Coarse panes in `PitchTracker` (pane array, counter-triggered pane kernel, hop combine); direct coarse code removed from the engine | `pitch` test (vibrato 3 cents, steady, silence, no alloc); new `pitch_test` case with the direct CMND as an oracle inside the test: pick within ±1 lag on ≥ 99.5 % of hops and the same voicing decision on ≥ 99 % over the vibrato tone and the voice clip; hard tune ≤ 4 cents; A/B autotune, octave, chain within -60 dB | hop block ~200k → ~45k; non-hop blocks +8k; pitch slot worst 288k → ~125k, avg 140k → ~100k |
| F2 | Predicted-lag fine panes with fallback, only on decision 11's trigger | as F1 plus fallback rate on the clips (near zero away from onsets); identical values on fallback | pitch slot worst → ~100k, flat |
| C1 | `core/costs.h` two columns from the bench, `measured = true`, rule 75 % / 95 %, `registry.cpp`, `test/firmware_test.cpp`, Mac app placement check, `2026-10-08-effect-registry-design.md` | `ctest` (firmware test checks the default layout against both sums); WeAct bench of the default layout: no late blocks on the voice clip | chain ~48 % avg, ~58 % worst (estimate) |
| C2 | This spec's "where it stands" table filled; CLAUDE.md spec list | — | — |
| T1 | Accuracy trial, not a cost step: fine search ±5 lags and a 5-point least-squares parabola against the 3-point one on the hard-tune clip and the 220 Hz gate. Kept only if hard tune improves; otherwise reverted and the result recorded here | hard tune ≤ 4 cents either way; `pitch` test | the 3-point parabola is the most biased standard sub-sample fit; whether it limits the 3.76 cents is unknown and cheap to find out |

## Progress (2026-10-08, host gates; WeAct bench pending, board off the bus)

| Step | Commit | ctest | A/B vs previous step | Other |
|---|---|---|---|---|
| U1 | 9514f5c | 17/17 | unison -39.7, chain -38.4 dB (old accumulator's error; new is -66.4 dB vs a double-phase reference) | LFO analytic 3.7e-5 samples; `sinTurns` 1.6e-7 |
| D1 | feaed12 | 17/17 | distortion -91.0, chain -96.5 dB | worst block should now equal average |
| M1 | c34dcaa | — | engine untouched | `gPitchFx` 101,736 B at 0x20000000; placement new after the zeroing |
| D2 | 1c57e38 | 17/17 | distortion -90.5, chain -92.9 dB | `tanhFast` 2.6e-7 max error |
| P1 | e54c85a | 17/17 | autotune -112.3, octave -117.6, chain -92.6 dB | hard tune 3.76 |
| F1 | 1d7b1e4 | 17/17 | all ten cases exact | oracle agreement 100 % on 562 + 750 hops; hard tune 3.76 |

Listening pairs for the owner: `build/listen/unison_{old_e06d829,new_u1}.wav`,
`build/listen/distortion_{old_feaed12,new_d2}.wav`. D3 and F2 wait on the bench.

## Tests

1. `unison_test`: analytic LFO delay check through a `delayAt(v)` test hook, at three
   Rate settings and Depth 0.8, 10 s. Existing rows stay.
2. `fastmath_test`: `sinTurns` absolute error under 1e-6 on 1e5 phases; `tanhFast`
   absolute error under 1e-5 on ±9, exact ±1 past the clamp, odd symmetry exact.
3. `pitch_test`: direct CMND oracle lives in the test, not the engine; agreement
   gates as F1. The vibrato case also reports the fallback count once F2 exists.
4. `autotune_test`: unchanged gates cover P1.
5. `firmware_test`: default layout within both sums; every card `measured`.
6. Existing gates unchanged: hard tune ≤ 4 cents, bypass exactness, `level_test`.

## Estimates after all steps (voice clip, all cards on)

| | Now | Expected |
|---|---|---|
| Chain average | 80.5 % | ~48 % |
| Chain worst block | 113.6 % | ~58 % |
| Pitch slot average | 21.9 % | ~14 % |
| Pitch slot worst | 45.0 % | ~19 % (F2 would make it ~15 %, flat) |
| Unison | 20.7 % | ~4 % |
| Distortion | 13.9 % | ~5 % |

These are code-reading estimates; the bench decides. A step that misses its expected
number by more than a third is examined before the next step starts.

## Ruled out

1. Spreading the YIN analysis across blocks by deferral (hard-tune gate, pitch pass
   decision 1). Panes are not a deferral: the pick still happens at the hop boundary
   on the same samples.
2. FFT coarse pass (three real 512-point transforms, d = E(0) + E(τ) − 2r). It shrinks
   the burst to ~60..80k but leaves it on the hop block; panes remove it, keep the
   exact statistic and need no FFT in `engine/`.
3. Sliding incremental YIN (subtract-on-evict): float drift after loud passages needs
   a periodic rebase; panes never subtract. Int64 sliding only if the pane array
   could not be placed, which it can.
4. Coarser decimation for the coarse pass (8 kHz): saves a third, needs a wider fine
   reach, and panes make the coarse cost small anyway.
5. Float phase accumulators with a rotor for the Unison LFO: at 0.2 Hz `cosf(inc)`
   rounds to 1.0f and the coupled form drifts in amplitude; the integer accumulator
   has neither problem.
6. Praat two-grain window rule L = 2·min(P, P/r): changes the sound at r > 1, and the
   default layout's Autotune stays within ±6 semitones (r ≤ 1.41, at most 4 live
   grains). Held for a listening test only if Octave A ever returns to a default layout.
7. Hand-scheduled VFMA-only or Q15 SMLALD coarse kernels: under panes the coarse
   kernel is ~8k per block, so halving it buys under 1 %; and `engine/` stays
   portable C++. Revisit only on evidence from the bench.
8. Two-term cosine recursion for the Hann window: saves two multiplies per grain-sample
   and drifts more than the rotor; not worth the state.
9. Early termination at the first CMND dip: moot once panes cover every lag.
10. Lowering the hard-tune gate or any A/B tolerance to make a step pass.
11. Dropping the Distortion 2x oversampling.

## Done when

`ctest` passes with the new cases; every A/B case is within its stated tolerance; the
owner has listened to the Unison and Distortion pairs; the WeAct runs the default layout
on the voice clip with no late blocks and both sums under their lines; `costs.h` holds
measured numbers for every card; T1's result is recorded whichever way it went.
