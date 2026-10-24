// Per-card CPU cost as integer percent of one audio block's cycle budget (kBlock samples at the
// sample rate, counted in cycles at 480 MHz), in two columns: the mean block and the worst
// single block. Each is rounded up, except Autotune's mean, which carries one extra point over
// its measured 13.0 %.
//
// Method: default layout on the voice clip with per-slot cycle counters, in two passes. The first
// pins every knob at its top position. The second catches loads pinned knobs cannot show: it
// switches Autotune on with the clip playing, and sweeps reverb MIX and SEMITONES end to end. A
// card's worst is the larger of the two passes. The worst figure is the lowest worst over repeated
// runs, so interrupt time landing in a block does not count. The pitch tracker is costed once,
// inside Autotune; Octave is the pitch slot with Octave on minus with it off. Chasm is costed in
// the reverb slot. The tail after the slots (comp, EQ, ceiling, BYPASS mix, conversion) has its
// own rows, read from the kStatComp and kStatEq buckets and the block total minus the slots.
//
// A layout is accepted when both column sums over its cards, plus the fixed term, stay under
// their lines. The trailing comments give the measured cycles, mean then worst; a row marked
// measured=false is an estimate until a board measures it.

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
constexpr Cost kInputGate{4, 5, true};     // 23.7k / 26.0k cycles
constexpr Cost kAutotune{14, 21, true};    // 83.2k / 131.9k, tracker included
constexpr Cost kOctave{5, 5, true};        // 25.7k / 27.2k, its own voice only
constexpr Cost kUnison{8, 8, true};        // 46.9k / 47.7k
constexpr Cost kSlapback{5, 5, true};      // 26.3k / 26.6k
constexpr Cost kDistortion{8, 10, true};   // 46.4k / 62.1k
constexpr Cost kGate{4, 5, true};          // 24.7k / 27.0k
constexpr Cost kReverbSpring{10, 10, true};  // 61.5k / 61.9k
constexpr Cost kReverbChasm{16, 16, true};   // 97.6k / 98.4k

// The tail every block spends whatever the layout.
constexpr Cost kOutputEq{3, 3, true};      // POLISH EQ and the sample conversion: 13.8k cycles
constexpr Cost kPolishComp{1, 1, false};   // two biquads and the fast log and exp per sample
constexpr Cost kSoftCeiling{1, 1, false};  // a compare per sample; tanh only past -2 dBFS
constexpr Cost kBypassMix{1, 1, false};    // the dry copy, the input energy sum, the BYPASS fade
constexpr Cost kFixed = kOutputEq + kPolishComp + kSoftCeiling + kBypassMix;

constexpr uint8_t kBudgetAvgPercent = 75;
constexpr uint8_t kBudgetWorstPercent = 95;

}  // namespace costs
