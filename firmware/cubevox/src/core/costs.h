// Per-card CPU cost as integer percent of one audio block's cycle budget (kBlock samples at the
// sample rate, counted in cycles at 480 MHz), in two columns: the mean block and the worst
// single block. Each is rounded up, except Autotune's mean, which carries one extra point over
// its measured 13.0 %.
//
// Method: default layout on the voice clip with every knob pinned at its top position and
// per-slot cycle counters. The worst figure is the lowest worst over repeated runs, so interrupt
// time landing in a block does not count. The pitch tracker is costed once, inside Autotune;
// Octave is the pitch slot with Octave on minus with it off. Chasm is costed in the reverb slot.
//
// A layout is accepted when both column sums over its cards, plus the fixed term, stay under
// their lines. The trailing comments give the measured cycles, mean then worst.

#pragma once

#include <stdint.h>

namespace costs {

struct Cost {
  uint8_t avg;    // mean block
  uint8_t worst;  // worst single block
  bool measured;  // false while the numbers are estimates, so the app shows "est."
};

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

// Output EQ and the block's sample conversion, spent whatever the layout: 13.8k cycles.
constexpr Cost kFixed{3, 3, true};

constexpr uint8_t kBudgetAvgPercent = 75;
constexpr uint8_t kBudgetWorstPercent = 95;

}  // namespace costs
