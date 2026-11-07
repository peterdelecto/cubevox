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
  pitchCards.autotune.on = false;
  pitchCards.octave.on = false;
  pitchCards.octave.mix = cv::kOctaveDefaultMix;  // until composeSnapshot reads its pot
  pitchCards.formant.on = false;
  crowd.on = false;
  growl.on = false;
  unison.on = false;
  slapback.on = false;
  distortion.on = false;
  lofi.on = false;
  bloom.on = false;
  reverb.on = false;
  applyPlacement(Card::ReverbSpring, *this);
  eq.on = true;
}
