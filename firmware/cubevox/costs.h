// Per-card CPU cost, integer percent of one audio frame at peak. Updated by hand per
// release from the bench table (CUBEVOX_BENCH). Every value is an estimate until a board
// measures it; the matching `measured` flag in registry.cpp flips when it does.
// The pitch tracker is costed once, inside Autotune; Octave adds only its own shifting.

#pragma once

#include <stdint.h>

namespace costs {

constexpr uint8_t kEmpty = 0;
constexpr uint8_t kInputGate = 2;
constexpr uint8_t kAutotune = 24;
constexpr uint8_t kOctave = 14;
constexpr uint8_t kUnison = 8;
constexpr uint8_t kSlapback = 3;
constexpr uint8_t kDistortion = 4;
constexpr uint8_t kGate = 2;
constexpr uint8_t kReverbSpring = 18;
constexpr uint8_t kReverbChasm = 14;

constexpr uint8_t kBudgetPercent = 85;

}  // namespace costs
