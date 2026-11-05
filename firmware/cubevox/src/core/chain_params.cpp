#include "core/chain_params.h"

#include "core/registry.h"

namespace {

// Input gate THRESHOLD until composeSnapshot applies the card's knobs; the middle of its range.
constexpr float kInputGateThresholdDb = 0.5f * (cv::kInputGateThresholdMinDb + cv::kInputGateThresholdMaxDb);

}  // namespace

ChainParams::ChainParams() {
  inputGate.on = true;
  inputGate.thresholdDb = kInputGateThresholdDb;
  inputGate.tuning = cv::inputGateTuning();
  gate.on = false;
  pitchFx.autotune.on = false;
  pitchFx.octave.on = false;
  pitchFx.octave.mix = cv::kOctaveDefaultMix;  // until composeSnapshot reads its pot
  pitchFx.formant.on = false;
  pitchFx.crowdOn = false;
  pitchFx.growlOn = false;
  crowd.on = false;
  growl.on = false;
  ringMod.on = false;
  strings.on = false;
  unison.on = false;
  slapback.on = false;
  distortion.on = false;
  lofi.on = false;
  bloom.on = false;
  reverb.on = false;
  applyPlacement(Card::ReverbSpring, *this);
  eq.on = true;
}
