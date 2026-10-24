// Display level of the input signal. The audio interrupt sums the energy of each block, and the
// UI tick turns the energy of one tick into a 0..1 level for the voice ribbon. The code is pure.

#pragma once

#include <stdint.h>

namespace level {

// The ribbon reaches full height at this RMS and stays flat at or below the floor, both in dB
// relative to full scale. Between them the level is linear in dB.
constexpr float kFullDbfs = -6.0f;
constexpr float kFloorDbfs = -50.0f;

// Exponent applied to the position between the floor and the full mark.
constexpr float kCurve = 0.6f;

// The level rises at once and falls with this time constant.
constexpr float kReleaseMs = 120.0f;

// A level below this reads as silence and snaps to 0.
constexpr float kSilence = 0.01f;

// The audio interrupt's tally since the last take. It holds the sum of the squared samples (full
// scale is 1.0) and the number of samples.
struct InputEnergy {
  float sumSquares = 0.0f;
  uint32_t samples = 0;
};

// Sum of the squares of `n` samples.
float sumSquares(const float* in, int n);

// RMS of the energy. 0 when it holds no samples.
float rms(const InputEnergy& energy);

// Maps an RMS amplitude (full scale is 1.0) onto 0..1.
float displayLevel(float amplitude);

// Follows the input level from one UI tick to the next.
class Meter {
 public:
  // Takes the energy of the tick that just ended, `dtMs` long. Returns the level to show.
  float update(const InputEnergy& energy, float dtMs);

 private:
  float level_ = 0.0f;
};

}  // namespace level
