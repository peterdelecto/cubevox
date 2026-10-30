// Display levels from the audio interrupt's energy tallies. The interrupt sums the energy of each
// block, once for the input and once for the output. The UI tick turns the energy of one tick into
// a 0..1 level. The input feeds the level bar, which reads absolute dB. The output feeds the voice
// ribbon, which scales itself to the recent peak. The code is pure.

#pragma once

#include <stdint.h>

namespace level {

// The input bar is empty at the floor and full at the top mark, both in dB relative to full
// scale. Between them the level is linear in dB.
constexpr float kFullDbfs = 0.0f;
constexpr float kFloorDbfs = -50.0f;

// The output ribbon shows the last 44 dB below a window top. The top jumps up to a louder tick at
// once and falls at this rate of real time, but never below the minimum, so a quiet tick cannot
// blow up to full height.
constexpr float kWindowSpanDb = 44.0f;
constexpr float kWindowMinTopDbfs = -30.0f;
constexpr float kWindowFallDbPerS = 10.0f;

// Exponent applied to the position inside the window.
constexpr float kCurve = 0.6f;

// A level rises at once and falls with this time constant.
constexpr float kReleaseMs = 120.0f;

// A level below this reads as silence and snaps to 0.
constexpr float kSilence = 0.01f;

// What toDbfs() returns for an amplitude of 0 or NaN.
constexpr float kNoSignalDbfs = -200.0f;

// The audio interrupt's tally since the last take. It holds the sum of the squared samples (full
// scale is 1.0) and the number of samples. The output is tallied the same way as the input.
struct InputEnergy {
  float sumSquares = 0.0f;
  uint32_t samples = 0;
};
using OutputEnergy = InputEnergy;

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

// Follows the output level from one UI tick to the next, scaled to a window below its recent peak.
class OutputMeter {
 public:
  // Takes the energy of the tick that just ended, `dtMs` long. Returns the level to show.
  float update(const OutputEnergy& energy, float dtMs);

  // The top of the window, in dBFS.
  float top() const { return top_; }

 private:
  float top_ = kWindowMinTopDbfs;
  float level_ = 0.0f;
};

}  // namespace level
