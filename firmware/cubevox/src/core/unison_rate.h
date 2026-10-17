// Unison RATE knob mapping. The box has no blend knob, so the Chorus-Double blend is fixed at its
// 0 % setting: no detune, with the swing at twice its base.

#pragma once

#include "engine/unison.h"

namespace unisonrate {

// LFO speed multiplier for a RATE position (0..100 %, clamped). Log-interpolated through three
// anchors: slow at 0 %, unity at 50 %, fast at 100 %.
float lfoScale(float ratePercent);

// Writes the LFO speeds and swing for a RATE position (0..100 %) at the fixed blend.
void apply(cv::UnisonTuning& tuning, float ratePercent);

}  // namespace unisonrate
