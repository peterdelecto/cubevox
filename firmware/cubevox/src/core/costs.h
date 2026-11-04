// Per-card CPU cost as integer percent of one audio block's cycle budget (kBlock samples at the
// sample rate, counted in cycles at 480 MHz), in two columns: the mean block and the worst
// single block. Each is rounded up. kTracker and kAutotune describe one measurement, the pitch
// slot with Autotune on, and only their sum stands for it; see kTracker.
//
// Method of record: firmware/stress.py runs the passes below on four layouts (default, crowd, chasm,
// crowd-chasm) on a clip bench image, two runs each, and gates every placed card against its row
// here. A row dated "stress run" takes its worst from the lowest run worst per layout.
//
// Method: default layout on the voice clip with per-slot cycle counters, in two passes. The first
// pins every knob at its top position. The second catches loads pinned knobs cannot show: it
// switches Autotune on with the clip playing, and sweeps reverb MIX and SEMITONES end to end. A
// card's worst is the larger of the two passes. The worst figure is the lowest worst over repeated
// runs, so interrupt time landing in a block does not count. Octave is the pitch slot with Octave
// on minus with it off. Chasm is costed in the reverb slot. The tail after the slots (comp, EQ,
// ceiling, BYPASS mix, conversion) has its own rows, read from the kStatComp and kStatEq buckets
// and the block total minus the slots. The kStatEq bucket holds both halves of POLISH EQ, the
// front half ahead of the slots and the back half after the comp.
//
// A host figure converts to a board estimate at 50.4k board cycles per host microsecond, which is
// 7.875 % of a block (the ratio Unison's host time gives against its 46.9k on the board). The
// pitch slot's code runs at fewer cycles per host microsecond, 24k to 29k (kTracker), so rows scaled
// from a board figure and rows converted at 50.4k are not at one ratio. kTracker states both, and
// the board's Crowd row (kCrowd) shows the pitch ratio holds for the grain code too. Echo's own
// ratio from the stress run of 2026-11-01 is 25.9k board cycles per host microsecond (13.2k board
// cycles over 0.509 host us, clang), half of 50.4k. Unison's 21.1k in the same run over the 0.93 host
// us its old 46.9k implies is 22.7k per host us. The 50.4k reads high against the stress run.
//
// The ratio belongs to a card, not to the box. The reverbs are serial chains of allpasses, which the
// host's out-of-order core hides and the in-order M7 pays for. The old Spring converted at 19.8k to
// 24.3k board cycles per host microsecond, the old Chasm at 16.1k (2.5 points per host microsecond),
// so 50.4k read their host time at 2.5 to 3 times the board. A reverb row is sized from the
// llvm-mca Cortex-M7 model of its per-sample loops, compiled by the firmware's GCC, which read the
// old Spring 13 % over its board figure (94.5k cycles a block against 83.4k) and the old Chasm 5 %
// over.
//
// Octave alone: PitchFx with only Octave on costs 0.32 host us per block (2026-11-02, Adam's clips,
// MIX 100 %, g++ 15 at -O2, host bench, minimum of 9 interleaved passes, +-0.02 us), which is 15.9k
// board cycles, 2.5 %, at 50.4k. The same case at MIX 0, where the voice parks, costs 0.11 us (5.4k,
// 0.9 %). That is the pitch stage's own work: the ring write, the per-sample mix and the crossfade
// gains. The voice is the other 0.21 us (10.5k, 1.6 %), which kOctave holds rounded up. The M7
// listing of the voice's loop is 65 instructions a sample, so the board may read lower. The pair
// behind kAutotune and kTracker was measured as the whole slot with Autotune on, so a layout with
// Autotune placed already carries the stage. Without Autotune, Octave and Formant pay it through
// kPitchStage, once.
//
// A layout is accepted when both column sums over its cards, plus the fixed term, the tracker once
// when Autotune, Crowd or Formant is placed, and kPitchStage once when Octave or Formant is placed
// without Autotune, stay under their lines. The trailing comments give the measured cycles, mean
// then worst; a row marked measured=false is an estimate until a board measures it.

#pragma once

#include <stdint.h>

namespace costs {

struct Cost {
  uint8_t avg;    // mean block
  uint8_t worst;  // worst single block
  bool measured;  // false while the numbers are estimates, so the app shows "est."
};

constexpr Cost operator+(Cost a, Cost b) {
  return {static_cast<uint8_t>(a.avg + b.avg), static_cast<uint8_t>(a.worst + b.worst),
          a.measured && b.measured};
}

constexpr Cost kEmpty{0, 0, true};
constexpr Cost kInputGate{3, 4, true};     // 17.7k to 17.9k / 21.6k to 25.1k, stress run 2026-11-01 (v2.00.81 and v2.00.82, all four layouts: mean 2.8 %, worst 3.4 to 3.9 %, lowest run worst per layout); the earlier 23.7k / 26.0k was the WeAct bench
constexpr Cost kAutotune{3, 5, false};     // est. the pair's host-scaled figure less kTracker: its voice, mix and fine pass; see kTracker. The worst carries one extra point because the stress run of 2026-11-01 read the pitch slot (Autotune, Octave and tracker) at 16.5 to 16.8 % worst against the 16 % the rows summed to
constexpr Cost kOctave{2, 2, false};       // est. from the host bench of 2026-11-02, its own voice only: 0.21 host us a block on g++ 15 (Octave at MIX 100 % less the same case at MIX 0), 10.5k board cycles at 50.4k, rounded up; see "Octave alone"
constexpr Cost kUnison{4, 5, true};        // 21.1k to 21.2k / 21.7k to 26.6k, stress run 2026-11-01 (v2.00.81 and v2.00.82, default and chasm layouts: mean 3.3 %, worst 3.4 to 4.2 %); the earlier 46.9k / 47.7k was the WeAct bench. The dry-at-unity rewrite (2026-11-02) costs no more: host 14.4 / 8.5 ns a sample at DEPTH 50 % (clang / gcc) against 14.5 / 9.0, and 14.7 / 11.6 with DEPTH moving against 14.4 / 12.2
constexpr Cost kSlapback{1, 3, true};      // 2.7k to 2.9k / 13.5k to 13.8k, stress run 2026-11-02 (v2.01.11, all seven layouts, TIME and AMOUNT swept, ten toggle flips: mean 0.4 to 0.5 %, worst 2.1 to 2.15 %, lowest run worst per layout); the host bench estimate of 6.4k to 8.3k worst missed the block after an enable
constexpr Cost kDistortion{5, 6, false};   // est. 2026-11-02, the llvm-mca Cortex-M7 model of the stage-major loops (xPack gcc 14.2.1, -O2) times 1.12 for both columns: 23.2k mean (a settled sample is 355 cycles, the knee costs the same at every level) and 29.8k costliest (DRIVE gliding up through the fade zone, 443 cycles a sample plus the sample-by-sample DRIVE step), 26.0k / 33.3k on the board, 4.1 % / 5.2 %. The stress run of 2026-11-01 read the tanh rail at 45.5k to 46.8k / 64.5k to 67.7k (7.1 to 7.3 % / 10.1 to 10.6 %); a board run with the Distortion sweeps owes the measured row
constexpr Cost kGate{3, 5, true};          // 18.5k to 18.9k / 21.3k to 25.9k, stress run 2026-11-01 (v2.00.81 and v2.00.82, all four layouts: mean 2.9 to 3.0 %, worst 3.3 to 4.05 %, lowest run worst per layout); the earlier 24.7k / 27.0k was the WeAct bench
constexpr Cost kReverbSpring{6, 7, true};   // 33.4k to 33.7k / 38.3k to 40.5k, stress run 2026-11-02 (v2.01.11, the six Spring layouts, MIX swept: mean 5.2 to 5.3 %, worst 6.0 to 6.3 %, lowest run worst per layout); the M7 model read 35.3k / 39.3k
constexpr Cost kReverbChasm{5, 6, true};    // 29.6k to 29.7k / 35.1k to 35.3k, stress run 2026-11-02 (v2.01.11, chasm and crowd-chasm layouts, MIX swept: mean 4.6 %, worst 5.5 %, lowest run worst per layout); the M7 model read 28.6k / 32.6k

// The pitch tracker, its history fill and its coarse analysis, which Autotune, Crowd and Formant
// share and a layout pays once. kTracker and kAutotune are one estimate split in two. The board
// measured the pair before the pitch CPU pass at 83.2k cycles for the mean block (13.0 %) and 131.9k
// for the worst (20.6 %). The CPU pass scaled those to 48.8k (7.6 %) and 66.9k (10.5 %) by the host
// ratio on Adam's clips with Autotune on at the defaults. The Autotune fix pass (2026-11-02, Adam's
// clips, hard tune, 64-sample blocks, -O2 with the firmware's float contraction, pooled medians of 9
// interleaved passes) scaled them again by its own host bench against main. Host us per block:
//
//   compiler      mean block         voiced hop block    tracker alone, after
//   g++ 15        1.794 -> 1.408     2.618 -> 1.910      0.867 (mean), 1.136 (hop)
//   Apple clang   1.629 -> 1.099     2.465 -> 1.632      0.550 (mean), 0.793 (hop)
//
// The M7 has no SIMD, and clang vectorizes the tracker's sums where g++ does not, so g++ stands for
// the board and the rows take its ratios: x0.785 on the mean block (38.3k cycles, 6.0 %) and x0.730 on
// the voiced hop block (48.8k, 7.6 %), about 27k board cycles per host us on the mean block and 26k on
// the hop block. The pair splits by the tracker's host share of Autotune on (Crowd on with Autotune
// off against Autotune on, same clips): mean 0.867 of 1.408 us (62 %), voiced hop block 1.136 of
// 1.910 us (59 %). The tracker takes 3.7 % and 4.5 %, rounded up to 4 and 5; Autotune's row takes the
// rest, 2.3 % and 3.1 %, rounded up to 3 and 4, plus the extra worst point its row explains. Both
// rows are estimates, and a board bench of the pair after the pass is owed. At the 50.4k ratio the
// tracker's 0.867 us would read 6.8 points mean and its 1.136 us 8.9 points worst, against the 4 and
// 5 held here.
constexpr Cost kTracker{4, 5, false};

// The pitch stage's own work when Octave or Formant runs and Autotune is not placed, which no card row
// holds. The term holds 4. Octave alone benches at 2.5 % (see "Octave alone"), so with kOctave's 2 the
// rows charge a layout with Octave and no Autotune 6 points against it, and the stage itself benches
// near 1. The held rows overcharge a layout with Octave or Formant and no Autotune by about 3 points
// until a board run measures Formant, whose own row is also an estimate. The stage does not vary
// block to block, so the worst equals the mean. The layout pays it once; with Autotune placed its rows
// carry it.
constexpr Cost kPitchStage{4, 4, false};

// The eight-copy card. The stress run of 2026-11-02 (v2.01.11) read Crowd's slot in the crowd and
// crowd-chasm layouts at 86.3k to 86.5k cycles mean (13.5 %), knobs at top, and 104.3k to 106.9k
// worst (16.3 to 16.7 %, lowest run worst per layout) with the DIVERGE and VOICES sweeps, with no
// late, overrun or dry block. The tracker runs in Autotune's slot there, so this row holds the card
// without the tracker. The same layouts read 79.0k / 99.7k to 101.3k on 2026-11-01 (v2.00.82),
// before VOICES became ten steps and before the Formant voice and its fix pass changed
// engine/formant_voice.h, which Crowd shares.
constexpr Cost kCrowd{14, 17, true};  // 86.3k to 86.5k / 104.3k to 106.9k, stress run 2026-11-02 (v2.01.11)

// The Formant card and the Formant follower, alternates for Octave in slot 3, and the Lo-fi and Chatter
// cards, alternates for Gate in slot 7. The follower pays the tracker and the pitch stage as Formant does. Formant's row is an estimate: the stress run reads it only together with the
// tracker and the pitch stage (formant-alone 29.2k to 29.4k / 52.8k to 54.3k, 4.6 % / 8.2 to 8.5 %,
// 2026-11-02, v2.01.14, against the 11 / 13 the three rows sum to).
constexpr Cost kFormant{3, 4, false};  // est.: one formant voice is about one MOB copy (1.5 %), plus the mix and the fine pass; the fine pass counts twice when Autotune is placed, about 1 point
constexpr Cost kFormantFollower{4, 5, false};  // est.: kFormant plus the follower. Host bench 2026-11-04 (Adam's clips, SENSITIVITY 50 %, Apple clang -O2, median of 9 interleaved passes, 2 runs; g++ 15 has no SDK here): 15.8 to 15.9 ns a sample at RANGE -12 st, the worst setting, where the grains play at half speed, against Formant's 14.8 at -7 st, at most 1.1 ns more, 3.7k board cycles a block at 50.4k, 0.59 %, rounded up
constexpr Cost kLofi{4, 5, true};      // 19.5k / 31.4k, stress run 2026-11-02 (v2.01.14, lofi layout, TONE and AMOUNT swept: mean 3.04 %, worst 4.9 %, lowest run worst); the estimate of 2 / 2 missed the coefficient ramp of a moving knob
constexpr Cost kBloom{2, 4, true};     // 11.7k to 12.0k / 22.9k to 23.4k, stress run 2026-11-02 (v2.01.14, chatter and heaviest layouts, LENGTH and AMOUNT swept: mean 1.8 to 1.9 %, worst 3.6 to 3.7 %, lowest run worst per layout); the host bench estimate was 3 / 5

// The Growl card, Distortion's alternate in slot 6, reads the tracker like Crowd, so a layout with it pays
// kTracker once. Its row is the card alone from the host bench of 2026-11-04 (g++ 15 at -O2 with the
// firmware's float contraction, Adam's three clips in 64-sample blocks, the pitch stage running for the
// ring and the tracker, minimum of 9 interleaved passes, mach_absolute_time per block): 0.478 host us a
// block at GROWL and DRIVE 100 % (0.430 at 50 / 50, 0.482 with both knobs swept end to end), and 0.666 us
// for the costliest block, its minimum over the passes. At 50.4k board cycles per host us that is 24.1k /
// 33.6k cycles, 3.8 % / 5.2 %, rounded up.
// With the throat move (2026-11-04), GROWL 100 % / DRIVE 50 %: 8.2 to 9.0 ns a sample, 26.5k to 29.0k
// cycles, 4.2 to 4.6 %, about 6.2 % worst block.
constexpr Cost kGrowl{5, 7, false};  // est. from that host bench; a board run owes the measured row

// The audition cards of 2026-11-04, from the host bench of Growl's method (Apple clang -O2 with the firmware's
// float contraction, Adam's three clips in 64-sample blocks, minimum of 9 passes, steady_clock around each
// call, 50.4k board cycles per host us). Ring mod is Distortion's second alternate in slot 6 and reads the
// tracker like Growl, so a layout with it pays kTracker once: 2.0 ns a sample, 129 ns a block, 6.5k cycles,
// 1.0 %, and 209 ns for the costliest block, 10.5k cycles, 1.6 %. Strings is Spring's second alternate in
// slot 8: 24.9 ns a sample, 1.60 us a block, 80.4k cycles, 12.6 %; the costliest block is a key change, when
// the old bank rings out beside the new one for 50 ms, 3.88 us, 195k cycles, 30.6 %. The 50.4k
// ratio was set on other cards, and this loop is a scalar recurrence that may not scale the same; a count of
// the loop (about 15 cycles a string a sample) says 2 % mean, 4 % worst. A board run owes the measured rows.
constexpr Cost kRingMod{2, 2, false};
constexpr Cost kStrings{13, 31, false};

// What every block spends whatever the layout: POLISH EQ's front half ahead of the slots, and the
// tail after them.
constexpr Cost kOutputEq{6, 7, false};     // est. 32.7k / 38k for both POLISH EQ halves and the sample conversion; 18.1k on the board for 7 biquads (9.2k measured, 8.9k est.) times the host ratio 1.55 for 9 biquads and the detector, plus 4.6k conversion; the worst adds the coefficient work of a block that switches on
constexpr Cost kPolishComp{1, 1, false};   // two biquads and the fast log and exp per sample
constexpr Cost kSoftCeiling{1, 1, false};  // a compare per sample; tanh only past -2 dBFS
constexpr Cost kBypassMix{1, 1, false};    // the dry copy, the input energy sum, the BYPASS fade
constexpr Cost kFixed = kOutputEq + kPolishComp + kSoftCeiling + kBypassMix;

constexpr uint8_t kBudgetAvgPercent = 75;
constexpr uint8_t kBudgetWorstPercent = 95;

}  // namespace costs
