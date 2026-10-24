#include "core/level.h"

#include <math.h>

namespace level {

float sumSquares(const float* in, int n) {
  float sum = 0.0f;
  for (int i = 0; i < n; ++i) sum += in[i] * in[i];
  return sum;
}

float rms(const InputEnergy& energy) {
  if (energy.samples == 0 || !(energy.sumSquares > 0.0f)) return 0.0f;  // also catches NaN
  return sqrtf(energy.sumSquares / static_cast<float>(energy.samples));
}

float displayLevel(float amplitude) {
  if (!(amplitude > 0.0f)) return 0.0f;  // also catches NaN
  const float db = 20.0f * log10f(amplitude);
  const float position = (db - kFloorDbfs) / (kFullDbfs - kFloorDbfs);
  if (!(position > 0.0f)) return 0.0f;
  return powf(position > 1.0f ? 1.0f : position, kCurve);
}

float Meter::update(const InputEnergy& energy, float dtMs) {
  const float now = displayLevel(rms(energy));
  level_ = now >= level_ ? now : now + (level_ - now) * expf(-dtMs / kReleaseMs);
  if (level_ < kSilence) level_ = 0.0f;
  return level_;
}

}  // namespace level
