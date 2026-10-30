#include "core/level.h"

#include <math.h>

namespace level {

namespace {

// Takes `now` at once when it is higher, else lets the level fall toward it over kReleaseMs.
float follow(float level, float now, float dtMs) {
  level = now >= level ? now : now + (level - now) * expf(-dtMs / kReleaseMs);
  return level < kSilence ? 0.0f : level;
}

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

}  // namespace

float windowBottom(float top) {
  const float bottom = top - kWindowSpanDb;
  return bottom > kWindowFloorDbfs ? bottom : kWindowFloorDbfs;
}

float sumSquares(const float* in, int n) {
  float sum = 0.0f;
  for (int i = 0; i < n; ++i) sum += in[i] * in[i];
  return sum;
}

float rms(const InputEnergy& energy) {
  if (energy.samples == 0 || !(energy.sumSquares > 0.0f)) return 0.0f;  // also catches NaN
  return sqrtf(energy.sumSquares / static_cast<float>(energy.samples));
}

float toDbfs(float amplitude) {
  if (!(amplitude > 0.0f)) return kNoSignalDbfs;  // also catches NaN
  return 20.0f * log10f(amplitude);
}

float displayLevel(float amplitude) {
  if (!(amplitude > 0.0f)) return 0.0f;  // also catches NaN
  return clamp01((toDbfs(amplitude) - kFloorDbfs) / (kFullDbfs - kFloorDbfs));
}

float Meter::update(const InputEnergy& energy, float dtMs) {
  level_ = follow(level_, displayLevel(rms(energy)), dtMs);
  return level_;
}

float OutputMeter::update(const OutputEnergy& energy, float dtMs) {
  const float db = toDbfs(rms(energy));
  if (db > top_) {
    top_ = db;
  } else {
    top_ -= kWindowFallDbPerS * dtMs / 1000.0f;
    if (top_ < kWindowMinTopDbfs) top_ = kWindowMinTopDbfs;
  }
  const float bottom = windowBottom(top_);
  const float position = clamp01((db - bottom) / (top_ - bottom));
  level_ = follow(level_, position > 0.0f ? powf(position, kCurve) : 0.0f, dtMs);
  return level_;
}

}  // namespace level
