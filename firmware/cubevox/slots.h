// Slot table: the eight fixed hardware slots and the effect assigned to each.
// The slot is hardware (two pots and a toggle). The effect, its knob meanings and the
// order are firmware, so reordering means editing kSlots in slots.cpp.

#pragma once

#include <stddef.h>
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

enum class Effect : uint8_t {
  InputGate,
  Autotune,
  Octave,
  Unison,
  Slapback,
  Distortion,
  Gate,
  Reverb,
};

enum class Param : uint8_t {
  GateThreshold,
  GateDecay,
  AutotuneKey,  // stepped, one position per kKeyRoot entry
  AutotuneResponse,
  OctaveSemitones,  // stepped, -24..+24
  OctaveMix,
  UnisonDepth,
  UnisonRate,
  SlapbackMix,
  SlapbackTime,
  DistortionDrive,
  DistortionTone,
  ReverbMix,
  ReverbTime,
};

struct Slot {
  Effect effect;
  Param knobA;
  Param knobB;
  uint32_t togglePin;  // pins:: constant
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

constexpr int kKeyPositions = 12;
constexpr int kSemitoneMin = -24;
constexpr int kSemitoneMax = 24;
constexpr int kSemitonePositions = kSemitoneMax - kSemitoneMin + 1;

const char* effectName(Effect e);
const char* paramName(Param p);

// 0 for a continuous knob, otherwise the number of positions it snaps to.
int paramSteps(Param p);

// Sets the engine field a knob drives. Continuous: value is 0..1. Stepped: value is the position index.
// The effect is needed because the input gate and the second gate share Param values.
void applyParam(Effect e, Param p, float value, ChainParams& out);

// Sets the on flag of the stage a slot owns.
void applyToggle(Effect e, bool on, ChainParams& out);

// Readout text for the OLED, for example "-24 dB" or "F / Dm".
void formatParam(Param p, float value, char* buf, size_t size);
