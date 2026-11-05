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

// Swing range at a multiplier of 1, and the multiplier above which the swing narrows with the speed so
// the copies' pitch swing stays capped.
constexpr float kSwingMinMs = 1.0f;
constexpr float kSwingMaxMs = 6.0f;
constexpr float kMaxSwingGrowth = 2.0f;

// Wet trim at RATE 100 %, ramped in dB from RATE 50 %. A faster, narrower sweep leaves the copies near
// their base delays, where they sum with the dry voice more than they did at RATE 50 % (the mean over
// Adam's clips climbs +0.25 to +0.61 dB with the wet itself 0.4 dB quieter). The trim holds the sum inside
// the level rule at every RATE.
constexpr float kTrimDbAtMax = -4.5f;

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
  const float narrow = m > kMaxSwingGrowth ? kMaxSwingGrowth / m : 1.0f;
  tuning.swingMinMs = kSwingMinMs * narrow;
  tuning.swingMaxMs = kSwingMaxMs * narrow;
  tuning.lfoHz[0] = kLfoBaseHzFirst * m;
  tuning.lfoHz[1] = kLfoBaseHzSecond * m;
  const float above = ratePercent > kRateMidPercent ? fminf(ratePercent, kRateMaxPercent) - kRateMidPercent : 0.0f;
  tuning.rateTrimDb = kTrimDbAtMax * above / (kRateMaxPercent - kRateMidPercent);
}

}  // namespace unisonrate
