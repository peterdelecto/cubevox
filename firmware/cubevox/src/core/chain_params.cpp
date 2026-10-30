#include "core/chain_params.h"

#include "core/registry.h"

namespace {

// Input gate THRESHOLD until composeSnapshot applies the card's knobs; the middle of its range.
constexpr float kInputGateThresholdDb = -55.0f;

// Octave MIX until composeSnapshot reads its pot.
constexpr float kOctaveStartMix = 0.10f;

}  // namespace

ChainParams::ChainParams() {
  inputGate.on = true;
  inputGate.thresholdDb = kInputGateThresholdDb;
  inputGate.tuning = cv::inputGateTuning();
  gate.on = false;
  pitchFx.autotune.on = false;
  pitchFx.octave.on = false;
  pitchFx.octave.mix = kOctaveStartMix;
  unison.on = false;
  slapback.on = false;
  distortion.on = false;
  reverb.on = false;
  applyPlacement(Card::ReverbSpring, *this);
  eq.on = true;
}
