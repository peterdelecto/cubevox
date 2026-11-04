// The Input gate's detector peak as a level in dBFS. The audio interrupt keeps the peak as a linear
// value, so the logarithm runs on the main loop. The status reply's `floor_dbfs` reads it.

#pragma once

#include <math.h>

// What a silent input reads as, and the lowest level reported.
constexpr float kFloorSilentDb = -120.0f;
constexpr float kFloorSilentLinear = 1e-6f;  // 10^(kFloorSilentDb / 20)

// The highest level reported. The detector reads nothing past +80 dBFS, so this is its own ceiling.
constexpr float kFloorTopDb = 80.0f;

// The level of a linear peak (full scale 1) in dBFS, between kFloorSilentDb and kFloorTopDb.
inline float peakDbfs(float peak) {
  if (!(peak > kFloorSilentLinear)) return kFloorSilentDb;  // also catches NaN
  const float db = 20.0f * log10f(peak);
  return db < kFloorTopDb ? db : kFloorTopDb;
}
