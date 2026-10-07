// Source: /Users/j/Documents/Claude/Projects/cubevox/emulator/macros.h (kUni* anchors, at(), unison() at blend 50 %), trimmed to the Unison RATE mapping.

#pragma once

#include <math.h>

#include "engine/unison.h"

namespace unisonrate {

constexpr float kBlendPercent = 50.0f;  // Chorus-Double centre; the box has no blend knob
constexpr float kMaxSwingGrowth = 2.0f;

// Log-interpolated lfo scale through 0.33 / 1.0 / 7.0 at RATE 0 / 50 / 100 %.
inline float lfoScale(float ratePercent) {
  const float r = ratePercent < 0.0f ? 0.0f : (ratePercent > 100.0f ? 100.0f : ratePercent);
  const bool upper = r > 50.0f;
  const float from = upper ? 1.0f : 0.33f;
  const float to = upper ? 7.0f : 1.0f;
  const float t = upper ? (r - 50.0f) / 50.0f : r / 50.0f;
  return from * powf(to / from, t);
}

// Writes the LFO speeds and swing for a RATE position (0..100 %), blend fixed at 50 %.
inline void apply(cv::UnisonTuning& t, float ratePercent) {
  const float m = lfoScale(ratePercent);
  const float swing = 1.0f * (m > kMaxSwingGrowth ? kMaxSwingGrowth / m : 1.0f);
  t.detuneCents[0] = 2.0f;
  t.detuneCents[1] = -2.0f;
  t.swingMinMs = 0.5f * swing;
  t.swingMaxMs = 3.0f * swing;
  t.lfoHz[0] = 0.60f * m;
  t.lfoHz[1] = 0.90f * m;
}

}  // namespace unisonrate
