#include "core/unison_rate.h"

#include <math.h>

namespace unisonrate {

namespace {

constexpr float kRateMidPercent = 50.0f;
constexpr float kRateMaxPercent = 100.0f;

// LFO speed multiplier at RATE 0 %, 50 % and 100 %.
constexpr float kScaleAtMin = 0.33f;
constexpr float kScaleAtMid = 1.0f;
constexpr float kScaleAtMax = 7.0f;

// Base LFO speeds of the two voices at a multiplier of 1.
constexpr float kLfoBaseHzFirst = 0.60f;
constexpr float kLfoBaseHzSecond = 0.90f;

// Swing range at a multiplier of 1, its scale, and the cap that stops fast rates from swinging wider.
constexpr float kSwingBaseMinMs = 0.5f;
constexpr float kSwingBaseMaxMs = 3.0f;
constexpr float kSwingScale = 2.0f;
constexpr float kMaxSwingGrowth = 2.0f;

}  // namespace

float lfoScale(float ratePercent) {
  float r = ratePercent;
  if (!(r > 0.0f)) r = 0.0f;
  if (r > kRateMaxPercent) r = kRateMaxPercent;
  const bool upper = r > kRateMidPercent;
  const float from = upper ? kScaleAtMid : kScaleAtMin;
  const float to = upper ? kScaleAtMax : kScaleAtMid;
  const float t = upper ? (r - kRateMidPercent) / (kRateMaxPercent - kRateMidPercent) : r / kRateMidPercent;
  return from * powf(to / from, t);
}

void apply(cv::UnisonTuning& tuning, float ratePercent) {
  const float m = lfoScale(ratePercent);
  const float swing = kSwingScale * (m > kMaxSwingGrowth ? kMaxSwingGrowth / m : 1.0f);
  tuning.swingMinMs = kSwingBaseMinMs * swing;
  tuning.swingMaxMs = kSwingBaseMaxMs * swing;
  tuning.lfoHz[0] = kLfoBaseHzFirst * m;
  tuning.lfoHz[1] = kLfoBaseHzSecond * m;
}

}  // namespace unisonrate
