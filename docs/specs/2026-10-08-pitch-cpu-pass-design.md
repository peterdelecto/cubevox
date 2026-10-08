# cubevox — pitch stage CPU pass (tracker, Autotune, Octave)

Date 2026-10-08. Owner: the full eight-card default layout must fit at 480 MHz;
rewriting effects is wanted. This spec covers the pitch stage (`engine/pitch.h`,
`engine/pitch_fx.h`, `engine/autotune.h`, `engine/psola.h`, `engine/shifters.h`,
`engine/octave*.h`). The other stages follow in their own passes. Budget per
64-sample block is 640,000 cycles.

## Where the cycles go today

Bench on the WeAct, Adam's voice clip, -O2, all cards on. Estimates from the code
against the measured slot totals (Autotune slot 273k on, 183k off, worst block 1,104k).

| Part | What runs | Cycles per block | When |
|---|---|---|---|
| YIN analysis | 171 lags × 256 MACs coarse, 9 × 1024 refine, one accumulator, no unroll | ~430k once per hop, ~108k averaged | every 4th block, always |
| Idle loop in `pitch_fx.h` | 2 sinf + 2 cosf crossfade, 2 expf via `smooth::coef` in the harmony and octave ticks, three low-pass rings | ~40k | every block, even all off |
| Autotune A voice | 8-grain loop, cosf window per grain, five `%` per cubic read, exp2f per sample | ~40k | Autotune on |
| Octave C voice | 2 grains, cosf each, exp2f, floorf, `%` wraps | ~25k | Octave on |

The YIN loop runs near 8 cycles per multiply-add where the M7 does 1 to 2. The
stage spends 40k per block doing nothing.

## Decisions

1. Analysis stays at the hop boundary (`kHop` 256). The loop gets four independent
   accumulators, unroll by four, pointer walks. Spreading the analysis across the
   four blocks of a hop is held in reserve; it adds 5.3 ms of pitch lag and would
   have to re-pass the hard-tune gate.
2. The tracker's input rings and decimation low-pass fill always (about 12 ops per
   sample). Analysis runs only while a consumer is on. Consumers: Autotune, Harmony,
   Octave A or B. Octave C needs no tracker, so the default layout runs the tracker
   only while Autotune is on. Enable is instant because the frame is already full.
3. All pitch cards off and crossfades settled: `process` copies in to out and writes
   only the shared input ring and the tracker buffers. The three per-engine low-pass
   rings fill only while the engine that reads them is selected (menu action; the
   switch resets them). Idle cost target under 2k per block.
4. Hann window by rotation recurrence per grain in `PsolaVoice::play` and
   `GrainShifter::tick`; one cosf at grain launch. Ring wraps by compare, `head`
   taken from the already-wrapped write count. Autotune's exp2f per sample becomes
   a polynomial exp2 with relative error under 1e-6 (0.002 cents), keeping the 1 ms
   response smoother exact. Octave and Harmony glide ratios are computed once per
   block and interpolated linearly (15 ms glide). Crossfade sin and cos once per
   block, interpolated; settled values are identical so bypass stays bit-exact.
   `smooth::coef(kSmoothSec)` becomes a constant computed once.
5. On enable the pitch voice resets so grains start clean, not from stale marks.
   Autotune keeps its snap to target on the first voiced frame after off.
6. The hard-tune gate in `test/autotune_test.cpp` stays at 4 cents RMS (3.8 today).
   A step that tips it is redone; the gate is never loosened.

## Steps

Each step: edit, `ctest` 15/15, `tools/engine_ab.sh e68171c` with the step's
tolerance, WeAct measure (`./term.py -s 0vm -s p -w 5`), commit with the average,
worst block and late count in the message.

| Step | Change | Tolerance | Expected |
|---|---|---|---|
| 1 | YIN loop rewrite, `coarse()` and `refine()` | pitch test within 3 cents (1.34 measured, same on the old loop); A/B -70 dB autotune, octave, chain (measured -77.6 / -75.7 / -71.4: reassociated sums flip a few borderline voiced decisions on the clip) | hop ~140k measured; stage 273k → 202k, worst block 1,104k → 818k |
| 2 | Tracker gating by consumer, instant enable | bit-exact while on (A/B vs step 1: 10 cases exact) | tracker off in the default layout unless Autotune is on |
| 3 | Stage idles: copy-through, per-engine LP rings, constant smoother coef | bit-exact | idle measured 5.5k (was 182.8k): warm buffers plus the three per-block prepares; all-off chain 365k → 174k |
| 4 | Crossfade gains at block rate; smoothers still step per sample | A/B vs step 3: 10 cases exact (renders do not toggle); bypass exact | stage 202k → 181k measured; chain avg 600k → 547k, part of that is Distortion and Spring moving 27k with no code change, read as flash placement noise |
| 5 | Spinning window, compare wraps, polynomial exp2; glide ratio recomputed only while the glide moves (bit-exact, better than the per-block lerp planned) | A/B vs step 4: autotune -106.5 dB, octave -108.6 dB, chain -92.0 dB (-60 allowed); hard tune 3.76 cents; exp2Fast 3.8e-7 relative, rotor 3.1e-6 over a 1372-sample grain | stage 181k → 147k measured; chain avg 547k → 518k, worst 790k → 733k. Wraps and the settled exp2 cache alone moved nothing: modulo by a constant was already a multiply |
| 6 | Fill `core/costs.h` from the bench, `measured=true` for Autotune and Octave | — | stage all-on ~80k, worst block under budget |

## Tests

1. New `test/pitch_test.cpp` (ctest `pitch`): a 5 Hz, ±30 cent vibrato tone on A3;
   every voiced hop after 100 ms within 3 cents of the analytic pitch, at least 95 %
   of hops voiced. Guards step 1 and any later latency change.
2. Step 5 adds unit checks: rotation window drift under 1e-4 over a two-period grain
   at `kMaxPeriod`; polynomial exp2 max relative error under 1e-6 over [-1, 1].
3. Existing gates hold unchanged: autotune bypass bit-exact, octave bypass exact at
   mix 0 / 0.5 / 1 and on=false, hard tune ≤ 4 cents, `level_test` rows.

## Open questions resolved

1. Spread the analysis or make it fast: fast first; spread only if the burst still
   does not fit, and then re-pass the hard-tune gate.
2. Off-state: skip the analysis, keep the input buffers warm; enable is instant.
   A full skip with 36 ms warm-up was the first proposal; the warm buffers cost
   under 1k per block and remove the warm-up.
3. Library math versus replacements: replacements, each with an error test.
   Keeping libm would spend 10 % of the chip for bit-exactness nothing downstream needs.
4. Window method: rotation recurrence over a lookup table (cheaper, no flash table).
5. Hard-tune gate: fixed at 4 cents.

## Done when

`ctest` passes with the new `pitch` test; the A/B guard reports every case within
its tolerance; the WeAct shows the Autotune slot under 90k all-on, under 2k all-off,
and no late blocks with the default layout running on the voice clip, or the
remaining overrun is attributed to the other stages by the per-slot stats.
