// Per-card CPU cost as integer percent of one audio block's cycle budget (kBlock samples at the
// sample rate, counted in cycles at 480 MHz), in two columns: the mean block and the worst
// single block. Each is rounded up. kTracker and kAutotune describe one measurement, the pitch
// slot with Autotune on, and only their sum stands for it; see kTracker.
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
// from a board figure and rows converted at 50.4k are not at one ratio; kTracker and kCrowd below
// state both.
//
// Octave alone: PitchFx with only Octave on costs 1.04 host us per block (2026-11-01, Adam's clips,
// MIX 100 %, host bench of the CPU pass, +-0.05 us), which is 52.4k board cycles, 8.2 %, at 50.4k.
// CPU pass 2 measured 51.7k (8.1 %) on the board for the same case before the CPU pass. kOctave
// holds 25.7k (4.0 %), so Octave alone pays about 26.7k cycles, 4 points, beyond its row.
// layoutCost counts none of it.
//
// A layout is accepted when both column sums over its cards, plus the fixed term and, once, the
// tracker when Autotune or Crowd is placed, stay under their lines. The trailing comments give the
// measured cycles, mean then worst; a row marked measured=false is an estimate until a board
// measures it.

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
constexpr Cost kInputGate{4, 5, true};     // 23.7k / 26.0k, the board's figures for the earlier tuning; the revised one runs at 0.97 of them on the host, so they bound it until a rerun
constexpr Cost kAutotune{5, 6, false};     // the pair's estimate less kTracker: its voice, mix and fine pass; see kTracker
constexpr Cost kOctave{5, 5, true};        // 25.7k / 27.2k, its own voice only
constexpr Cost kUnison{8, 8, true};        // 46.9k / 47.7k
constexpr Cost kSlapback{5, 5, true};      // 26.3k / 26.6k
constexpr Cost kDistortion{8, 10, true};   // 46.4k / 62.1k
constexpr Cost kGate{4, 5, true};          // 24.7k / 27.0k
constexpr Cost kReverbSpring{15, 15, true};  // ~92k: three springs, from the board's boot worst 107.4k against the two-half engine's 72.0k (1.49x of 61.9k); two-pass bench pending
constexpr Cost kReverbChasm{16, 16, true};   // 97.6k / 98.4k

// The pitch tracker, its history fill and its coarse analysis, which Autotune and Crowd share and a
// layout pays once. kTracker and kAutotune are one estimate split in two. The pair was measured on
// the board before the pitch CPU pass at 83.2k cycles for the mean block (13.0 %) and 131.9k for
// the worst (20.6 %); this estimate scales those by the host ratio the CPU pass measured on Adam's
// clips with Autotune on at the defaults, host us per block: mean 2.83 before, 1.66 after (x0.587);
// voiced hop block 5.58 before, 2.83 after (x0.507). That gives 48.8k cycles (7.6 %) for the mean
// and 66.9k (10.5 %) for the worst, which is about 29k board cycles per host us on the mean block
// and 24k on the voiced hop block. The pair splits by the tracker's host share of Autotune on
// (Crowd on with Autotune off against Autotune on, same clips, 2026-11-01): mean 0.68 of 1.60 us
// (43 %), voiced hop block 1.21 of 2.75 us (44 %). The tracker takes 3.2 % and 4.6 %, rounded up
// to 4 and 5; Autotune's row takes the rest, 4.4 % and 5.9 %, rounded up to 5 and 6. Both rows are
// estimates, and a board bench of the pair after the pass is owed. At the 50.4k ratio the tracker's
// 0.68 us would read 5.4 points mean and its 1.21 us 9.5 points worst, against the 4 and 5 held
// here.
constexpr Cost kTracker{4, 5, false};

// Estimate of 2026-11-01 from the host for the eight-copy card: Crowd::process at 2.40 us per block
// (VOICES 80 %, WIDTH 50 %, default tuning, Autotune off) is 18.9 points at the 50.4k ratio, rounded
// up to 19. The parts are the eight copies' grains (four tuned, four raised), the shared tone shelf,
// the low cut, the gap rule and the mix; the tracker is kTracker. The worst is the mean plus the 2
// points the earlier row carried; no worst block was measured. The tracker's row above sits at the
// pair's board ratio of about 29k cycles per host us and this row at 50.4k; at 29k Crowd's 2.40 us
// would read 11 points. Both pitch audits measured about 28k for grain and tracker code, so this row
// probably overstates, and a board bench is owed before it is trusted.
constexpr Cost kCrowd{19, 21, false};

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
