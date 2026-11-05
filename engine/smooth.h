#pragma once

#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"

// One-pole smoothing shared by the engines.

namespace cv {
namespace smooth {

constexpr float kSmoothSec = 0.020f;
constexpr float kSnap = 1e-6f;

// x limited to [0, 1]. NaN reads as 0.
constexpr float clamp01(float x) { return clampf(x, 0.0f, 1.0f); }

// The fraction of its distance to the target that a one-pole with time constant sec keeps over n
// samples; 0 or less means instant. This is the one closed form for any n and any time constant.
inline float keep(float sec, int n) {
  return sec <= 0.0f ? 0.0f : expf(-static_cast<float>(n) / (sec * kSampleRate));
}

// Coefficient of a one-pole that steps once per block of n samples, with time constant sec.
inline float blockCoef(float sec, int n) { return 1.0f - keep(sec, n); }

// Per-sample coefficient for time constant sec; 0 or less means instant.
inline float coef(float sec) { return blockCoef(sec, 1); }

// The 20 ms coefficient and its keep factor over one kBlock, as literals so a sample loop reads them as
// immediates. smooth_test pins them to coef(kSmoothSec) and keep(kSmoothSec, kBlock). The coefficient
// is 1 - exp(-1/960) rounded in float, which sits 9.6e-6 under the exact value (a 20.0002 ms time
// constant), and the stages' hashes pin that value.
constexpr float kSmoothCoef = 0.00104111433f;
constexpr float kSmoothKeepBlock = 0.935506999f;

// One step toward target. It lands exactly on the target once within kSnap or once a step no
// longer moves the float, so a settled smoother holds its target bit for bit.
inline float step(float x, float target, float a) {
  const float next = x + a * (target - x);
  return next == x || fabsf(target - next) < kSnap ? target : next;
}

// x moved toward target by at most `delta`, a straight line. It lands on the target exactly.
inline float toward(float x, float target, float delta) {
  if (x < target) return target - x <= delta ? target : x + delta;
  return x - target <= delta ? target : x - delta;
}

// x after n steps toward target. A smoother that already sits on its target returns at once.
inline float advance(float x, float target, float a, int n) {
  if (x == target) return target;
  for (int i = 0; i < n; ++i) x = step(x, target, a);
  return x;
}

// Two smoothers that share a coefficient, stepped together so their dependent chains overlap. One that
// already sits on its target drops out, and the other runs alone.
inline void advance(float& x, float xTarget, float& y, float yTarget, float a, int n) {
  if (x == xTarget || y == yTarget) {
    x = advance(x, xTarget, a, n);
    y = advance(y, yTarget, a, n);
    return;
  }
  for (int i = 0; i < n; ++i) {
    x = step(x, xTarget, a);
    y = step(y, yTarget, a);
  }
}

// x after n steps toward target in closed form and without the loop. `a` is the per-sample coefficient
// and `decay` is keep(sec, n) for the same time constant. It lands on the target by the loop's rule,
// within kSnap of it or once a step would no longer move the float, so it lands on the block where the
// loop's own stall does.
inline float advanceBy(float x, float target, float a, float decay) {
  const float next = target + (x - target) * decay;
  const bool stalled = next + a * (target - next) == next;
  return stalled || fabsf(target - next) < kSnap ? target : next;
}

// advanceBy for kBlock steps of the 20 ms smoother.
inline float advanceBlock(float x, float target) { return advanceBy(x, target, kSmoothCoef, kSmoothKeepBlock); }

}  // namespace smooth
}  // namespace cv
