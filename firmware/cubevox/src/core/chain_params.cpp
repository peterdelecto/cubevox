#include "core/chain_params.h"

#include "core/registry.h"

namespace {

// Input gate tuning the panel never sets: it drops 12 dB when closed and holds 100 ms, so gaps
// between syllables do not chatter. THRESHOLD and DECAY hold these values only until
// composeSnapshot applies the card's knobs.
constexpr float kInputGateThresholdDb = -12.0f;
constexpr float kInputGateRangeDb = -12.0f;
constexpr float kInputGateHoldMs = 100.0f;
constexpr float kInputGateReleaseMs = 10.0f;

// Octave MIX until composeSnapshot reads its pot.
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
  pitchFx.octave.on = false;
  pitchFx.octave.mix = kOctaveStartMix;
  unison.on = false;
  slapback.on = false;
  distortion.on = false;
  reverb.on = false;
  applyPlacement(Card::ReverbSpring, *this);
  eq.on = true;
}
