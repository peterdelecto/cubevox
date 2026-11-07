// The input level meter's display level from the audio block's energy. The block sums the energy of
// its input, and the UI tick turns the energy of one tick into a level in absolute dB. The code is
// pure.

#pragma once

#include <stdint.h>

namespace level {

// The input meter is empty at the floor and full at the top mark, both in dB relative to full
// scale. Between them the level is linear in dB.
constexpr float kFullDbfs = 0.0f;
constexpr float kFloorDbfs = -50.0f;

// A level on the input meter rises at once and falls with this time constant.
constexpr float kReleaseMs = 120.0f;

// A level below this reads as silence and snaps to 0.
constexpr float kSilence = 0.01f;

// What toDbfs() returns for an amplitude of 0 or NaN.
constexpr float kNoSignalDbfs = -200.0f;

// The input energy since the last take. It holds the sum of the squared samples (full scale is 1.0)
// and the number of samples, tallied by the audio interrupt.
struct InputEnergy {
  float sumSquares = 0.0f;
  uint32_t samples = 0;
};

// Sum of the squares of `n` samples.
float sumSquares(const float* in, int n);

// RMS of the energy. 0 when it holds no samples.
float rms(const InputEnergy& energy);

// dB relative to full scale for an RMS amplitude, kNoSignalDbfs when it is 0 or NaN.
float toDbfs(float amplitude);

// Maps an RMS amplitude (full scale is 1.0) onto 0..1, linear in dB from the floor to the top mark.
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
