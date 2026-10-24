#include "core/output_stage.h"

#include <string.h>

namespace {

constexpr uint32_t kFloatExponent = 0x7F800000u;  // all ones for an infinity or a NaN

}  // namespace

bool guardBlock(float* x, int n) {
  float sum = 0.0f;
  for (int i = 0; i < n; ++i) sum += x[i];
  uint32_t bits;
  memcpy(&bits, &sum, sizeof bits);
  if ((bits & kFloatExponent) != kFloatExponent) return true;
  for (int i = 0; i < n; ++i) x[i] = 0.0f;
  return false;
}

GainRamp OutputGain::next(float trim, float fadeFrom, float fadeTo, int n) {
  const float from = trim_ * fadeFrom;
  const float to = trim * fadeTo;
  trim_ = trim;
  return {from, (to - from) / static_cast<float>(n)};
}
