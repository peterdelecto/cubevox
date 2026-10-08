#include "core/chain_params.h"

namespace {

// Input gate live-stage defaults (emulator/main.cpp, owner 2026-10-01).
constexpr float kInputGateThresholdDb = -12.0f;
constexpr float kInputGateRangeDb = -12.0f;
constexpr float kInputGateHoldMs = 100.0f;
constexpr float kInputGateReleaseMs = 10.0f;

// Octave defaults the emulator opens with.
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
  pitchFx.harmony.on = false;  // Harmony has no card (owner 2026-10-08)
  pitchFx.octave.on = false;
  pitchFx.octave.engine = kOctaveEngineC;
  pitchFx.octave.mix = kOctaveStartMix;
  unison.on = false;
  slapback.on = false;
  distortion.on = false;
  reverb.on = false;
  reverb.engine = cv::kReverbSpringB;
  eq.on = true;
}
