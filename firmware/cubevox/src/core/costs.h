// Per-card CPU cost as integer percent of one audio block (640,000 cycles at 480 MHz),
// rounded up, two columns: the mean block and the worst single block. Measured on the WeAct
// H743 bench (2026-10-09, CPU pass 3 end, main at 71fe7bc): default layout on the voice clip, every knob
// pinned at its top position, per-slot cycle counters, worst taken as the lowest worst over
// repeated runs so interrupt time landing in a block does not count. The pitch tracker is
// costed once, inside Autotune; Octave is the pitch slot with Octave on minus with it off.
// Chasm was measured in the reverb slot in place of Spring. A layout is accepted when both
// column sums over its cards, plus the fixed term, stay under their lines.

#pragma once

#include <stdint.h>

namespace costs {

struct Cost {
  uint8_t avg;    // mean block
  uint8_t worst;  // worst single block
};

constexpr Cost kEmpty{0, 0};
constexpr Cost kInputGate{4, 5};     // 23.7k / 26.0k cycles
constexpr Cost kAutotune{14, 21};    // 83.2k / 131.9k, tracker included
constexpr Cost kOctave{5, 5};        // 25.7k / 27.2k, its own voice only
constexpr Cost kUnison{8, 8};        // 46.9k / 47.7k
constexpr Cost kSlapback{5, 5};      // 26.3k / 26.6k
constexpr Cost kDistortion{8, 10};   // 46.4k / 62.1k
constexpr Cost kGate{4, 5};          // 24.7k / 27.0k
constexpr Cost kReverbSpring{10, 10};  // 61.5k / 61.9k
constexpr Cost kReverbChasm{16, 16};   // 97.6k / 98.4k

// Output EQ and the block's sample conversion, spent whatever the layout: 13.8k cycles.
constexpr Cost kFixed{3, 3};

constexpr uint8_t kBudgetAvgPercent = 75;
constexpr uint8_t kBudgetWorstPercent = 95;

}  // namespace costs
