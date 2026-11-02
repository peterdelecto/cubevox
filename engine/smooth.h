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

// Coefficient for a smoother that steps once per block of n samples, with time constant sec.
inline float blockCoef(float sec, int n) {
  return sec <= 0.0f ? 1.0f : 1.0f - expf(-static_cast<float>(n) / (sec * kSampleRate));
}

// The 20 ms coefficient, computed once; per-sample callers must not call coef().
inline const float kSmoothCoef = coef(kSmoothSec);

// One step toward target. It lands exactly on the target once within kSnap or once a step no
// longer moves the float, so a settled smoother holds its target bit for bit.
inline float step(float x, float target, float a) {
  const float next = x + a * (target - x);
  return next == x || fabsf(target - next) < kSnap ? target : next;
}

// x after n steps toward target.
inline float advance(float x, float target, float a, int n) {
  for (int i = 0; i < n; ++i) x = step(x, target, a);
  return x;
}

// What a smoother keeps of its distance to the target over one kBlock of steps: (1 - kSmoothCoef)^kBlock.
inline const float kSmoothKeepBlock = powf(1.0f - kSmoothCoef, static_cast<float>(kBlock));

// x after kBlock steps toward target, in closed form and without the loop. It lands on the target by the
// step's rule: within kSnap of it, or once a block no longer moves the float. The loop's per-sample stall
// comes sooner, so the final approach takes a few blocks more than advance() does.
inline float advanceBlock(float x, float target) {
  const float next = target + (x - target) * kSmoothKeepBlock;
  return next == x || fabsf(target - next) < kSnap ? target : next;
}

// Two smoothers that share a coefficient, stepped together so their dependent chains overlap.
inline void advance(float& x, float xTarget, float& y, float yTarget, float a, int n) {
  for (int i = 0; i < n; ++i) {
    x = step(x, xTarget, a);
    y = step(y, yTarget, a);
  }
}

}  // namespace smooth
}  // namespace cv
