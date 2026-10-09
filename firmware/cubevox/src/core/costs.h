// Per-card CPU cost as integer percent of one audio block (640,000 cycles at 480 MHz),
// rounded up, two columns: the mean block and the worst single block. Measured on the WeAct
// H743 bench (2026-10-08, main at d0f45b2): default layout on the voice clip, every knob
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
constexpr Cost kInputGate{4, 5};     // 24.0k / 26.9k cycles
constexpr Cost kAutotune{14, 21};    // 84.0k / 133.0k, tracker included
constexpr Cost kOctave{5, 5};        // 25.7k / 27.2k, its own voice only
constexpr Cost kUnison{9, 9};        // 55.2k / 57.1k
constexpr Cost kSlapback{5, 5};      // 27.0k / 27.8k
constexpr Cost kDistortion{13, 18};  // 80.6k / 109.5k
constexpr Cost kGate{4, 5};          // 24.5k / 26.7k
constexpr Cost kReverbSpring{11, 11};  // 68.7k / 69.1k
constexpr Cost kReverbChasm{16, 16};   // 97.6k / 98.4k

// Output EQ and the block's sample conversion, spent whatever the layout: 13.8k cycles.
constexpr Cost kFixed{3, 3};

constexpr uint8_t kBudgetAvgPercent = 75;
constexpr uint8_t kBudgetWorstPercent = 95;

}  // namespace costs
