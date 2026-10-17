#include "core/chain_params.h"

#include "core/registry.h"

namespace {

// Live-vocal gate: opens at -12 dB, drops 12 dB when closed, holds 100 ms so gaps between
// syllables do not chatter, and releases in 10 ms so word endings are not chopped.
constexpr float kInputGateThresholdDb = -12.0f;
constexpr float kInputGateRangeDb = -12.0f;
constexpr float kInputGateHoldMs = 100.0f;
constexpr float kInputGateReleaseMs = 10.0f;

// The granular octave engine (OctaveParams::engine 2) is the one the box ships. It starts with a
// light mix so the second voice sits under the dry vocal.
constexpr int kOctaveEngineC = 2;
constexpr float kOctaveStartMix = 0.10f;

}  // namespace

ChainParams::ChainParams() {
  inputGate.on = true;
  inputGate.thresholdDb = kInputGateThresholdDb;
  inputGate.tuning.rangeDb = kInputGateRangeDb;
  inputGate.tuning.holdMs = kInputGateHoldMs;
  inputGate.tuning.releaseMs = kInputGateReleaseMs;
  gate.on = false;
  pitchFx.autotune.on = false;
  pitchFx.harmony.on = false;  // Harmony has no card, so its engine stays off
  pitchFx.octave.on = false;
  pitchFx.octave.engine = kOctaveEngineC;
  pitchFx.octave.mix = kOctaveStartMix;
  unison.on = false;
  slapback.on = false;
  distortion.on = false;
  reverb.on = false;
  applyPlacement(Card::ReverbSpring, *this);
  eq.on = true;
}
