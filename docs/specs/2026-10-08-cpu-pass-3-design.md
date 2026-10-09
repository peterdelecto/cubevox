# cubevox — CPU pass 3 (Spring B chains, Unison voices, Distortion restore)

Date 2026-10-08. Owner: go with the recommendation after the reverb research note
(Spring B interleave, block-rate sqrt and exp, Unison voice interleave, Distortion
state hoist). Follows `2026-10-08-cpu-pass-2-design.md`; its gates, bench protocol
and commit rules carry over. Budget per 64-sample block is 640,000 cycles (100 %).

## Where it stands (c4744ef, voice clip, all cards on, knobs pinned at top)

| Card | Avg | Worst | Percent |
|---|---|---|---|
| Spring | 68.7k | 69.1k | 11 / 11 |
| Unison | 55.2k | 57.1k | 9 / 9 |
| Distortion | 80.6k | 109.5k | 13 / 18 |
| Chain | 402.2k | 480.0k | 62.8 / 75.0, late 0 |

## Findings behind the pass

1. Regression. d0f45b2 (pass 2 step D3) replaced `engine/distortion.h` with a copy that
   predates D1 and D2 plus the ring histories. HEAD computes two `expf` and the makeup
   (`sqrtf` and two divides) every sample, recomputes the treble shelf every sample while
   TONE glides, and calls `tanhf` in the rail. The -60 dB A/B gate cannot see this:
   `tanhFast` sits within 1e-5 of `tanhf` and the gains differ only during the drive ramp.
   This is why Distortion measures 80.6k against D2's expectation of under 35k and why
   its worst block sits 29k over its average.
2. Spring B runs its two 24-section stretched-allpass chains one after the other, each
   in its own loop. A section is two dependent FMAs, so one chain is latency-bound at
   roughly 6 to 8 cycles per section. The halves are independent within a sample: half 2
   reads its line before half 1's tap is written, and the shortest line is 16 samples.
   The reverb research note (owner, 2026-10-08) ranks interleaving independent chains
   as the first exact cut.
3. Spring B evaluates `sqrtf(1 - g²)` every sample and two `expf` every sample while
   DWELL moves. Both depend on a smoother alone.
4. Unison runs a per-voice loop with an out-of-line read, steps the depth smoother per
   sample with two snap compares, and recomputes swing and wet gain per sample. The two
   voices are independent.

## Decisions

1. Order: D4, S1, U2, then D5 only on its trigger, then C3. Each step is its own commit
   carrying its A/B and bench numbers. The A/B reference for a step is the previous
   step's commit. No gate is loosened.
2. D4 restores D1 and D2 exactly as 1c57e38 had them, on top of the D3 ring: tuning
   applied on change, treble shelf once per block from the block-start TONE, stage gains
   and makeup at the block edges interpolated linearly, `tanhFast` in the rail. The
   `tanhFast` test case is restored if the D3 base dropped it.
3. S1. The two dispersers run as one pair: a single section loop advances both halves
   per iteration with the same arithmetic in the same order per chain, so the result is
   bit-exact. The loop gain g and the input scale `sqrt(1 - g²)` are evaluated at the
   block edges from the decay smoother and interpolated linearly; dwell drive and
   compensation likewise. Exact once settled, so the A/B (settled from the first block)
   is exact and `level_test` holds.
4. U2. Both voices are computed in one straight-line body per sample. Per-voice phases,
   increments and the write position live in locals for the block. Depth is stepped n
   times at the block start (the same sequence as today), swing and wet gain are taken
   at the block edges and interpolated linearly. When both voices are parked for the
   whole block (shift phase and increment 0) the block runs a path with no per-sample
   branches; the crossfaded path stays for any nonzero detune. The cubic read's
   arithmetic is unchanged, so the A/B is exact.
5. D5, held. `__restrict` on the block pointers and filter state hoisted into locals for
   the block loop. Trigger: D4's bench leaves Distortion above 45k average.
6. Process rule, from finding 1: an agent's worktree branches from main as it stands
   when the agent starts, the merge is the diff of exactly the files in its brief, and
   that diff is read against main before the commit. The A/B gate is not a guard against
   losing an earlier step.

## Steps

Each step: edit, `cmake --build build -j && ctest --test-dir build`,
`tools/engine_ab.sh <previous step commit> --tol <dB>`, WeAct bench
(`BOARD=weact ./build.sh`, `./flash.sh`, then `./term.py -s 0kvm -s p -w 12` once after
the flash; `-s 0m -s p -w 12` after), commit with the numbers. Worst is the lowest over
three runs.

| Step | Change | Gate | Expected |
|---|---|---|---|
| D4 | Distortion restore (decision 2) | A/B distortion and chain within -60 dB (the tanh swap and the ramp), bypass exact, `distortion_test`, `fastmath_test` tanh case | 80.6k → ~40k avg; worst within 10 % of avg |
| S1 | Disperser pair, block-edge g, input scale, drive and comp | A/B spring and chain exact; `spring_b_test`, `level_test` | 68.7k → ~55k |
| U2 | Unison voices interleaved, locals, block-edge swing and wet gain, parked block path | A/B unison and chain exact; `unison_test` analytic LFO case within 0.05 samples | 55.2k → under 25k |
| D5 | Only on decision 5's trigger | bit-exact | — |
| C3 | `core/costs.h` from the bench, this spec's tables, CLAUDE.md spec list | `firmware_test` both sums | chain ~55 % / ~65 % |

## Progress

| Step | Commit | ctest | A/B vs previous step | Bench |
|---|---|---|---|---|
| D4 | 285600f | 17/17 | distortion -90.0, chain -92.8 dB (the tanh swap), all other cases exact | pending, board off the bus |
| S1 | this commit | 17/17 | all ten cases exact | pending |

## Ruled out

1. Running the disperser chain at fs/4 with crossovers (research note rank 1). It
   changes the sound: Spring B passes the whole band through the dispersers and the
   sweep edge would move from 4 kHz to 3 kHz. Not before S1, and only with a listen.
2. Section-outer block form for the disperser (six-way parallelism from the stretch).
   It needs a 70-float buffer per section, 13 KB, and the pair interleave gets most of
   the gain. Held unless S1 saves under 8k.
3. Block-edge LFO sine with linear interpolation in Unison: at Rate 100 % (20 Hz) the
   error reaches a sample against the 0.05-sample analytic gate.
4. Everything in pass 2's ruled-out list.

## Done when

`ctest` passes; every A/B case is within its stated tolerance; the WeAct runs the default
layout on the voice clip with no late blocks and both sums under their lines;
`costs.h` holds the new numbers; the D5 trigger has been read either way.
