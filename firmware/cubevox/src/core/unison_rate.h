// Source: /Users/j/Documents/Claude/Projects/cubevox/emulator/macros.h (kUni* anchors, at(), unison() at blend 0 %), trimmed to the Unison RATE mapping.

#pragma once

#include <math.h>

#include "engine/unison.h"

namespace unisonrate {

// The box has no blend knob, so the Chorus-Double blend sits at Adam's 0 % (macros.h kDefaultPos):
// no fixed detune, swing at twice the base (kUniDetune and kUniSwingScale at 0 %).
constexpr float kDetuneCents = 0.0f;
constexpr float kSwingScale = 2.0f;
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

// Writes the LFO speeds and swing for a RATE position (0..100 %) at the fixed blend.
inline void apply(cv::UnisonTuning& t, float ratePercent) {
  const float m = lfoScale(ratePercent);
  const float swing = kSwingScale * (m > kMaxSwingGrowth ? kMaxSwingGrowth / m : 1.0f);
  t.detuneCents[0] = kDetuneCents;
  t.detuneCents[1] = 0.0f - kDetuneCents;  // +0, not -0, at no detune
  t.swingMinMs = 0.5f * swing;
  t.swingMaxMs = 3.0f * swing;
  t.lfoHz[0] = 0.60f * m;
  t.lfoHz[1] = 0.90f * m;
}

}  // namespace unisonrate
