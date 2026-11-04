#include "core/output_stage.h"

#include <string.h>

namespace {

constexpr float kGuardMeanSquare = 1.0e6f;  // +60 dBFS RMS

}  // namespace

bool guardBlock(float* x, int n) {
  // Four sums run side by side, because one serial sum waits on each add.
  float s0 = 0.0f;
  float s1 = 0.0f;
  float s2 = 0.0f;
  float s3 = 0.0f;
  int i = 0;
  for (; i + 4 <= n; i += 4) {
    s0 += x[i] * x[i];
    s1 += x[i + 1] * x[i + 1];
    s2 += x[i + 2] * x[i + 2];
    s3 += x[i + 3] * x[i + 3];
  }
  for (; i < n; ++i) s0 += x[i] * x[i];
  const float sum = (s0 + s1) + (s2 + s3);

  // Squares are never negative, so the sum's bits order like its value. A NaN of either sign and an
  // infinity both sort above the bound.
  const float bound = static_cast<float>(n) * kGuardMeanSquare;
  uint32_t sumBits;
  uint32_t boundBits;
  memcpy(&sumBits, &sum, sizeof sumBits);
  memcpy(&boundBits, &bound, sizeof boundBits);
  if (sumBits < boundBits) return true;
  for (int j = 0; j < n; ++j) x[j] = 0.0f;
  return false;
}

GainRamp OutputGain::next(float trim, float fadeFrom, float fadeTo, int n) {
  const float from = trim_ * fadeFrom;
  const float to = trim * fadeTo;
  trim_ = trim;
  return {from, (to - from) / static_cast<float>(n)};
}
