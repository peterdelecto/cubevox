#include "core/level.h"

#include <math.h>

#include "engine/dsp.h"
#include "engine/smooth.h"

namespace level {

namespace {

// Takes `now` at once when it is higher, else lets the level fall toward it over kReleaseMs.
float follow(float level, float now, float dtMs) {
  level = now >= level ? now : now + (level - now) * expf(-dtMs / kReleaseMs);
  return level < kSilence ? 0.0f : level;
}

}  // namespace

// Four sums run side by side, because one serial sum waits on each add. The meter reads the total
// to display precision.
float sumSquares(const float* in, int n) {
  float s0 = 0.0f;
  float s1 = 0.0f;
  float s2 = 0.0f;
  float s3 = 0.0f;
  int i = 0;
  for (; i + 4 <= n; i += 4) {
    s0 += in[i] * in[i];
    s1 += in[i + 1] * in[i + 1];
    s2 += in[i + 2] * in[i + 2];
    s3 += in[i + 3] * in[i + 3];
  }
  for (; i < n; ++i) s0 += in[i] * in[i];
  return (s0 + s1) + (s2 + s3);
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
  return cv::smooth::clamp01((toDbfs(amplitude) - kFloorDbfs) / (kFullDbfs - kFloorDbfs));
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
  if (db < floor_) {
    floor_ = db > kWindowFloorDbfs ? db : kWindowFloorDbfs;
  } else {
    floor_ += kWindowFloorRiseDbPerS * dtMs / 1000.0f;
  }
  const float highest = top_ - kWindowFloorUnderTopDb;
  if (floor_ > highest) floor_ = highest;
  if (db < bottom()) return 0.0f;
  return cv::smooth::clamp01(cv::dbToGain(db - top_));
}

}  // namespace level
