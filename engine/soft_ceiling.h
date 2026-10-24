#pragma once

#include <cmath>

#include "engine/common.h"
#include "engine/fastmath.h"

// Output ceiling: the last stage before the DAC. A sample inside +-kSoftCeilingKnee (-2 dBFS)
// passes bit for bit. Above the knee a tanh shoulder takes over with the same value, slope and
// curvature, so the bend has no corner, and it levels off at full scale. The result is finite and
// never past +-1.0, and a NaN becomes 0. Stateless, so one call per sample in any loop.

namespace cv {

constexpr float kSoftCeilingKnee = 0.79432823f;  // 10^(-2/20)
constexpr float kSoftCeilingRoom = 1.0f - kSoftCeilingKnee;
constexpr float kSoftCeilingInvRoom = 1.0f / kSoftCeilingRoom;

CV_INLINE float softCeiling(float x) {
  const float a = std::fabs(x);
  if (a <= kSoftCeilingKnee) return x;
  if (std::isnan(a)) return 0.0f;

  float y = kSoftCeilingKnee + kSoftCeilingRoom * tanhFast((a - kSoftCeilingKnee) * kSoftCeilingInvRoom);
  if (y > 1.0f) y = 1.0f;
  return x < 0.0f ? -y : y;
}

}  // namespace cv
