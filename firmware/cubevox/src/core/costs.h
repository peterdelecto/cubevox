// Per-card CPU cost as integer percent of one audio block's cycle budget (kBlock samples at the
// sample rate, counted in cycles at 480 MHz), in two columns: the mean block and the worst
// single block. Each is rounded up, except Autotune's mean, which carries one extra point over
// its measured 13.0 %.
//
// Method: default layout on the voice clip with per-slot cycle counters, in two passes. The first
// pins every knob at its top position. The second catches loads pinned knobs cannot show: it
// switches Autotune on with the clip playing, and sweeps reverb MIX and SEMITONES end to end. A
// card's worst is the larger of the two passes. The worst figure is the lowest worst over repeated
// runs, so interrupt time landing in a block does not count. The pitch tracker is costed once, as
// kTracker, and Autotune's row holds the rest of its measured total; Octave is the pitch slot with
// Octave on minus with it off. Chasm is costed in the reverb slot. The tail after the slots (comp,
// EQ, ceiling, BYPASS mix, conversion) has its own rows, read from the kStatComp and kStatEq
// buckets and the block total minus the slots. The kStatEq bucket holds both halves of POLISH EQ,
// the front half ahead of the slots and the back half after the comp.
//
// A host figure converts to a board estimate at 50.4k board cycles per host microsecond, which is
// 7.875 % of a block (the ratio Unison's host time gives against its 46.9k on the board).
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
constexpr Cost kAutotune{5, 5, true};      // 83.2k / 131.9k less kTracker: its voice and what the fine pass adds
constexpr Cost kOctave{5, 5, true};        // 25.7k / 27.2k, its own voice only
constexpr Cost kUnison{8, 8, true};        // 46.9k / 47.7k
constexpr Cost kSlapback{5, 5, true};      // 26.3k / 26.6k
constexpr Cost kDistortion{8, 10, true};   // 46.4k / 62.1k
constexpr Cost kGate{4, 5, true};          // 24.7k / 27.0k
constexpr Cost kReverbSpring{15, 15, true};  // ~92k: three springs, from the board's boot worst 107.4k against the two-half engine's 72.0k (1.49x of 61.9k); two-pass bench pending
constexpr Cost kReverbChasm{16, 16, true};   // 97.6k / 98.4k

// The pitch tracker, its history fill and its coarse analysis, which Autotune and Crowd share and a
// layout pays once. Estimate of 2026-10-31 from the host after the CPU pass: PitchFx at 1.04 us per
// block with Crowd on is 8.2 points, rounded up to 9. Autotune's row loses the same, so its total
// holds. The worst is the mean plus the 7 points that Autotune's row showed between its mean and its
// worst, taken as the tracker's hop block; no worst block was measured. A board bench is still owed.
constexpr Cost kTracker{9, 16, false};

// Estimate of 2026-11-01 from the host for the eight-copy card: Crowd::process at 2.40 us per block
// (VOICES 80 %, WIDTH 50 %, default tuning, Autotune off) is 18.9 points, rounded up to 19. The parts
// are the eight copies' grains (four tuned, four raised), the shared tone shelf, the low cut, the gap
// rule and the mix; the tracker is kTracker. The worst is the mean plus the 2 points the earlier row
// carried; no worst block was measured. Both pitch audits measured about 28k board cycles per host
// us for grain and tracker code against the 50.4k used here, so a board bench is owed before the row
// is trusted.
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
