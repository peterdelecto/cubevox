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
// and the block total minus the slots. The kStatEq bucket holds both halves of VOX EQ, the
// front half ahead of the slots and the back half after the comp.
//
// A host figure converts to a board estimate at 50.4k board cycles per host microsecond, which is
// 7.875 % of a block. The pitch slot's code runs at fewer cycles per host microsecond, 24k to 29k
// (kTracker), so rows scaled from a board figure and rows converted at 50.4k are not at one ratio.
// kTracker states both, and the board's Crowd row (kCrowd) shows the pitch ratio holds for the grain
// code too. Echo's own ratio from the stress run of 2026-11-01 is 25.9k board cycles per host
// microsecond (13.2k board cycles over 0.509 host us, clang), half of 50.4k, so the 50.4k reads high
// against the stress run.
//
// A row marked false is an estimate. firmware/stress.py turns one true: it reads the card's slot
// counters across its sweeps and toggle flips, and the fixed path across its BYPASS, POLISH COMP and
// VOX EQ flips, and gates each against its row. The row then takes the stress run's mean and its
// lowest run worst, rounded up, with the date and the layouts in its comment.
//
// The ratio belongs to a card, not to the box. The reverbs are serial chains of allpasses, which the
// host's out-of-order core hides and the in-order M7 pays for. The old Spring converted at 19.8k to
// 24.3k board cycles per host microsecond, the old Chasm at 16.1k (2.5 points per host microsecond),
// so 50.4k read their host time at 2.5 to 3 times the board. A reverb row is sized from the
// llvm-mca Cortex-M7 model of its per-sample loops, compiled by the firmware's GCC, which read the
// old Spring 13 % over its board figure (94.5k cycles a block against 83.4k) and the old Chasm 5 %
// over.
//
// Octave alone: the engine with only Octave on costs 0.251 host us per block (2026-11-05, Adam's clips,
// MIX 100 %, g++ 15 at -O2, host bench, minimum of 9 interleaved passes, +-0.02 us), which is 12.6k
// board cycles, 2.0 %, at 50.4k: the ring write, the voice, the per-sample mix and the crossfade gains.
// kOctave holds all of it, and a layout that places Octave pays no other term for it. The M7 listing of
// the voice's loop is 65 instructions a sample, so the board may read lower. Every engine carries its own
// mix, so no row charges for work another card does.
//
// A layout is accepted when both column sums over its cards, plus the fixed term and the trackers, stay
// under their lines. Autotune pays one tracker. A Formant or Apraxia card directly behind it reads that
// tracker, and so do the Mob and Deepthroat cards anywhere behind it. A Formant or Apraxia card anywhere
// else, and a Mob or Deepthroat card with no Autotune ahead, pay a tracker of their own. The trailing
// comments give the measured cycles, mean then worst; a row marked measured=false is an estimate until a
// board measures it.

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
constexpr Cost kInputGate{1, 3, true};     // 5.7k to 6.0k / 15.2k to 17.8k, stress run 2026-11-05 (v2.01.40, default, lofi, chatter and chasm layouts: mean 0.9 %, worst 2.4 to 2.8 %)
constexpr Cost kAutotune{3, 5, false};     // est. the pair's host-scaled figure less kTracker: its voice, its crossfade and the fine pass; see kTracker. The worst carries one extra point because the stress run of 2026-11-01 read the pitch slot (Autotune, Octave and tracker) at 16.5 to 16.8 % worst against the 16 % the rows summed to
constexpr Cost kOctave{2, 2, false};       // est. from the host bench of 2026-11-05, the whole engine: 0.251 host us a block on g++ 15 (MIX 100 %), 12.6k board cycles at 50.4k, rounded up; see "Octave alone"
constexpr Cost kUnison{2, 3, true};        // 10.5k / 13.7k to 15.7k, stress run 2026-11-05 (v2.01.40, default, lofi, chatter and chasm layouts: mean 1.6 %, worst 2.1 to 2.5 %)
constexpr Cost kSlapback{1, 3, true};      // 2.7k to 2.9k / 13.5k to 13.8k, stress run 2026-11-02 (v2.01.11, all seven layouts, TIME and AMOUNT swept, ten toggle flips: mean 0.4 to 0.5 %, worst 2.1 to 2.15 %, lowest run worst per layout); the host bench estimate of 6.4k to 8.3k worst missed the block after an enable
constexpr Cost kDistortion{4, 6, true};    // 24.1k to 24.2k / 32.8k to 33.0k, stress run 2026-11-05 (v2.01.40, default, lofi, chatter and chasm layouts: mean 3.8 %, worst 5.1 to 5.2 %)
constexpr Cost kGate{3, 4, true};          // 13.4k to 13.6k / 20.5k to 20.7k, stress run 2026-11-05 (v2.01.40, default, lofi, chatter and chasm layouts: mean 2.1 %, worst 3.2 %)
constexpr Cost kReverbSpring{6, 7, true};   // 33.4k to 33.7k / 38.3k to 40.5k, stress run 2026-11-02 (v2.01.11, the six Spring layouts, MIX swept: mean 5.2 to 5.3 %, worst 6.0 to 6.3 %, lowest run worst per layout); the M7 model read 35.3k / 39.3k
constexpr Cost kReverbChasm{5, 6, true};    // 29.6k to 29.7k / 35.1k to 35.3k, stress run 2026-11-02 (v2.01.11, chasm and crowd-chasm layouts, MIX swept: mean 4.6 %, worst 5.5 %, lowest run worst per layout); the M7 model read 28.6k / 32.6k

// The pitch tracker, its history fill and its coarse analysis, which Autotune runs, and which the Formant
// engine directly behind it, Crowd and Growl read. A layout pays one for Autotune's, and one more for each
// Formant or Apraxia card not directly behind Autotune and each Mob or Deepthroat card with no Autotune
// ahead, since those run a tracker of their own. kTracker and kAutotune are one estimate split in two. The board
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

// The eight-copy card. The stress run of 2026-11-02 (v2.01.11) read Crowd's slot in the crowd and
// crowd-chasm layouts at 86.3k to 86.5k cycles mean (13.5 %), knobs at top, and 104.3k to 106.9k
// worst (16.3 to 16.7 %, lowest run worst per layout) with the DIVERGE and VOICES sweeps, with no
// late, overrun or dry block. The tracker runs in Autotune's slot there, so this row holds the card
// without the tracker. The same layouts read 79.0k / 99.7k to 101.3k on 2026-11-01 (v2.00.82),
// before VOICES became ten steps and before the Formant voice and its fix pass changed
// engine/formant_voice.h, which Crowd shares.
constexpr Cost kCrowd{14, 17, true};  // 86.3k to 86.5k / 104.3k to 106.9k, stress run 2026-11-02 (v2.01.11)

// The Formant card and Apraxia, alternates for Octave in slot 3, and the Lo-fi and Chatter cards, alternates
// for Gate in slot 7. Apraxia runs the Formant engine, so it pays the tracker as Formant does. The row holds
// the voice, the base and the mix. Directly behind Autotune the card cuts from Autotune's input and runs one
// voice (the corrected share), and a second while Autotune's crossfade moves the voice from one share to the
// other; settled off it runs the one voice at ratio 1, and its base is a ring read. Formant's row is an
// estimate: the stress run read the card only together with the tracker and the old pitch stage (formant-alone
// 29.2k to 29.4k / 52.8k to 54.3k, 4.6 % / 8.2 to 8.5 %, 2026-11-02, v2.01.14, against the 11 / 13 the three
// rows summed to).
constexpr Cost kFormant{4, 7, false};  // est. from the host bench of 2026-11-05 (Adam's clips, -3 st, MIX 100 %, Autotune on, Apple clang -O2, minimum of 15 passes per block; g++ 15 has no SDK here): the card adds 0.47 host us a block over Autotune alone, 3.7 % at 50.4k, rounded up to 4; its worst 1 % of blocks add 0.74 us, 5.8 %, and a block of Autotune's crossfade runs a second voice, about 0.85 us, 6.7 %, rounded up to 7
constexpr Cost kFormantFollower{5, 8, false};  // est.: kFormant plus the follower. Host bench 2026-11-04 (Adam's clips, SENSITIVITY 50 %, Apple clang -O2, median of 9 interleaved passes, 2 runs; g++ 15 has no SDK here): 15.8 to 15.9 ns a sample at RANGE -12 st, the worst setting, where the grains play at half speed, against Formant's 14.8 at -7 st, at most 1.1 ns more, 3.7k board cycles a block at 50.4k, 0.59 %, rounded up
constexpr Cost kLofi{2, 5, true};          // 11.1k / 26.6k, stress run 2026-11-05 (v2.01.40, default, lofi, chatter and chasm layouts: mean 1.7 %, worst 4.1 %)
constexpr Cost kBloom{2, 4, true};     // 11.7k to 12.0k / 22.9k to 23.4k, stress run 2026-11-02 (v2.01.14, chatter and heaviest layouts, LENGTH and AMOUNT swept: mean 1.8 to 1.9 %, worst 3.6 to 3.7 %, lowest run worst per layout); the host bench estimate was 3 / 5

// The Growl card, Distortion's alternate in slot 6, reads Autotune's tracker like Crowd, and pays a tracker
// of its own with no Autotune ahead. Its row is the card's slot measured on the box. The host bench of 2026-11-04 had put it
// at 3.8 % / 5.2 %, and 4.2 to 4.6 % mean with the throat move.
constexpr Cost kGrowl{5, 7, true};  // 25.7k / 42.7k, stress run 2026-11-05 (v2.01.51-bench, throat layout: mean 4.0 %, worst 6.7 %)

// What every block spends whatever the layout: VOX EQ's front half ahead of the slots, and the
// tail after them. The stress run of 2026-11-05 measured VOX EQ at ON+CUTS (both halves) and POLISH COMP, and
// stress.py gates their buckets against kPolishEq and kPolishComp. The run read the rest of the fixed
// path, SOFT CEILING and BYPASS mix, at about 0.9 % mean combined. Those two rows stay estimates.
// stress.py gates the whole fixed path against kFixed, the sum of the four rows.
constexpr Cost kPolishEq{2, 3, true};      // 10.9k / 15.0k to 15.1k, stress run 2026-11-05 (v2.01.40, default, lofi, chatter and chasm layouts: mean 1.7 %, worst 2.4 %)
constexpr Cost kPolishComp{3, 4, true};    // 14.6k to 15.4k / 18.7k to 20.9k, stress run 2026-11-05 (v2.01.40, default, lofi, chatter and chasm layouts: mean 2.3 to 2.4 %, worst 2.9 to 3.3 %)
constexpr Cost kSoftCeiling{1, 1, false};  // est.: a compare per sample; tanh only past -2 dBFS
constexpr Cost kBypassMix{1, 1, false};    // est.: the dry copy, the input energy sum, the BYPASS fade
constexpr Cost kFixed = kPolishEq + kPolishComp + kSoftCeiling + kBypassMix;

constexpr uint8_t kBudgetAvgPercent = 75;
constexpr uint8_t kBudgetWorstPercent = 95;

}  // namespace costs
