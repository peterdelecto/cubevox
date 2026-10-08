// Slot hardware: the eight fixed slots, each two pots on the mux and a toggle.
// Which card sits in a slot is the layout (registry.h), not hardware.

#pragma once

#include <stdint.h>

#include "engine/distortion.h"
#include "engine/gate.h"
#include "engine/pitch_fx.h"
#include "engine/polish.h"
#include "engine/reverb.h"
#include "engine/slapback.h"
#include "engine/unison.h"

constexpr int kSlotCount = 8;
constexpr int kPotCount = 2 * kSlotCount;  // POT01..POT16 on mux I0..I15

struct Slot {
  uint32_t togglePin;   // pins:: constant
  uint8_t muxChannelA;  // 0-based mux input, POTnn is channel nn - 1
  uint8_t muxChannelB;
};

extern const Slot kSlots[kSlotCount];

// Everything the audio block needs from the panel and menu. Built by the main loop,
// published whole to the audio interrupt (see audioPublish).
struct ChainParams {
  cv::GateParams inputGate;
  cv::PitchFxParams pitchFx;
  cv::UnisonParams unison;
  cv::SlapbackParams slapback;
  cv::DistortionParams distortion;
  cv::GateParams gate;
  cv::ReverbParams reverb;
  cv::PolishParams eq;
  float unisonRatePercent = 50.0f;
  float inputGainDb = 0.0f;   // digital trim ahead of the chain (MIC adds back the 1/4" pad)
  float outputGainDb = 0.0f;

  ChainParams();
};
