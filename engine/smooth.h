#pragma once

#include <cmath>

#include "engine/common.h"

// One-pole smoothing shared by the pitch stages.

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

// Lands exactly on its target, so settled bypass is bit-exact.
inline float step(float x, float target, float a) {
  x += a * (target - x);
  return fabsf(target - x) < kSnap ? target : x;
}

}  // namespace smooth
}  // namespace cv
