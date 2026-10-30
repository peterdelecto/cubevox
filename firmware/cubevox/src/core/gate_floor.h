// The Input gate's detector peak as a level in dBFS. The audio interrupt keeps the peak as a linear
// value, so the logarithm runs on the main loop. LEARN and the status reply's `floor_dbfs` both
// read it.

#pragma once

#include <math.h>

// What a silent input reads as, and the lowest level reported.
constexpr float kFloorSilentDb = -120.0f;

// The level of a linear peak (full scale 1) in dBFS, never below kFloorSilentDb.
inline float peakDbfs(float peak) {
  if (!(peak > 1e-6f)) return kFloorSilentDb;  // also catches NaN
  const float db = 20.0f * log10f(peak);
  return db > kFloorSilentDb ? db : kFloorSilentDb;
}
