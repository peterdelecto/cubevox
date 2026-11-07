#include "core/output_stage.h"

#include <string.h>

#include "core/level.h"

namespace {

constexpr float kGuardMeanSquare = 1.0e6f;  // +60 dBFS RMS

}  // namespace

bool guardBlock(float* x, int n) {
  const float sum = level::sumSquares(x, n);

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
