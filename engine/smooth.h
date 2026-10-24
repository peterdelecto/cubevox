#pragma once

#include <cmath>

#include "engine/common.h"

// One-pole smoothing shared by the engines.

namespace cv {
namespace smooth {

constexpr float kSmoothSec = 0.020f;
constexpr float kSnap = 1e-6f;

inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// Per-sample coefficient for time constant sec; 0 or less means instant.
inline float coef(float sec) {
  return sec <= 0.0f ? 1.0f : 1.0f - expf(-1.0f / (sec * kSampleRate));
}

// The 20 ms coefficient, computed once; per-sample callers must not call coef().
inline const float kSmoothCoef = coef(kSmoothSec);

// One step toward target. It lands exactly on the target once within kSnap or once a step no
// longer moves the float, so a settled smoother holds its target bit for bit.
inline float step(float x, float target, float a) {
  const float next = x + a * (target - x);
  return next == x || fabsf(target - next) < kSnap ? target : next;
}

}  // namespace smooth
}  // namespace cv
