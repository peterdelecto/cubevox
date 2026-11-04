// Unison RATE knob mapping: the LFO speeds and the swing for a RATE position. It runs in composeSnapshot
// from loop(), so its powf never enters the audio interrupt, and the engine holds no copy of it.

#pragma once

#include "engine/unison.h"

namespace unisonrate {

// LFO speed multiplier for a RATE position (0..100 %, clamped). Log-interpolated through three
// anchors: slow at 0 %, unity at 50 %, fast at 100 %.
float lfoScale(float ratePercent);

// Writes the LFO speeds and swing for a RATE position (0..100 %).
void apply(cv::UnisonTuning& tuning, float ratePercent);

}  // namespace unisonrate
