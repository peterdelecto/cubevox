// Display levels from the audio interrupt's energy tallies. The interrupt sums the energy of each
// block, once for the input and once for the output. The UI tick turns the energy of one tick into
// a 0..1 level. The input feeds the level meter, which reads absolute dB. The output feeds the voice
// ribbon, which reads amplitude against the recent peak and hides steady noise. The code is pure.

#pragma once

#include <stdint.h>

namespace level {

// The input meter is empty at the floor and full at the top mark, both in dB relative to full
// scale. Between them the level is linear in dB.
constexpr float kFullDbfs = 0.0f;
constexpr float kFloorDbfs = -50.0f;

// The output ribbon reads each tick's amplitude over the amplitude of a window top. The top jumps
// up to a louder tick at once and falls at this rate of real time, but never below the minimum, so
// a quiet tick cannot blow up to full height.
constexpr float kWindowMinTopDbfs = -30.0f;
constexpr float kWindowFallDbPerS = 10.0f;

// A floor follows the quietest recent tick, so steady noise draws as the line. The floor drops to
// a quieter tick at once, but never under the silence floor. It rises at this rate of real time,
// but never closer than kWindowFloorUnderTopDb to the top, so the loudest recent tick always
// draws. A tick under the floor plus kWindowCutoffMarginDb reads as silence.
constexpr float kWindowFloorDbfs = -60.0f;
constexpr float kWindowFloorRiseDbPerS = 3.0f;
constexpr float kWindowFloorUnderTopDb = 12.0f;
constexpr float kWindowCutoffMarginDb = 6.0f;

// A level on the input meter rises at once and falls with this time constant.
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

// Reads the output level of each UI tick as its amplitude over the recent peak's. A tick has no
// release, so each one stands on its own.
class OutputMeter {
 public:
  // Takes the energy of the tick that just ended, `dtMs` long. Returns the level to show, which is
  // 10^((tick - top) / 20) held to 0..1, or 0 for a tick under the cutoff.
  float update(const OutputEnergy& energy, float dtMs);

  // The top of the window, in dBFS.
  float top() const { return top_; }

  // The floor, in dBFS.
  float floor() const { return floor_; }

  // The cutoff, in dBFS. A tick under it reads as silence.
  float bottom() const { return floor_ + kWindowCutoffMarginDb; }

 private:
  float top_ = kWindowMinTopDbfs;
  float floor_ = kWindowFloorDbfs;
};

}  // namespace level
