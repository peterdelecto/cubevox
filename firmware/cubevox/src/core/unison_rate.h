// Unison RATE knob mapping: the LFO speeds and the swing for a RATE position. The swing sits at
// twice its base, the chorus voicing the owner tuned.

#pragma once

#include "engine/unison.h"

namespace unisonrate {

// LFO speed multiplier for a RATE position (0..100 %, clamped). Log-interpolated through three
// anchors: slow at 0 %, unity at 50 %, fast at 100 %.
float lfoScale(float ratePercent);

// Writes the LFO speeds and swing for a RATE position (0..100 %).
void apply(cv::UnisonTuning& tuning, float ratePercent);

}  // namespace unisonrate
