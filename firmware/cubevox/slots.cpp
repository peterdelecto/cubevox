#include "slots.h"

#include <math.h>
#include <stdio.h>

#include "engine/harmony.h"
#include "pins.h"
#include "unison_rate.h"

// POT15 is KEY and POT16 is SEMITONES (hardware/PINMAP.md). The other fourteen pots fill
// POT01..POT14 left to right, so KEY and SEMITONES sit out of left-to-right order.
// Channel numbers are 0-based mux inputs. POT01-08 and POT16 are channel nn - 1; POT09-15 are
// re-mapped to match the board's west-row order (hardware/PINMAP.md, 2026-10-08).
constexpr uint8_t kPot01 = 0;
constexpr uint8_t kPot02 = 1;
constexpr uint8_t kPot03 = 2;
constexpr uint8_t kPot04 = 3;
constexpr uint8_t kPot05 = 4;
constexpr uint8_t kPot06 = 5;
constexpr uint8_t kPot07 = 6;
constexpr uint8_t kPot08 = 7;
constexpr uint8_t kPot09 = 13;
constexpr uint8_t kPot10 = 14;
constexpr uint8_t kPot11 = 9;
constexpr uint8_t kPot12 = 11;
constexpr uint8_t kPot13 = 8;
constexpr uint8_t kPot14 = 10;
constexpr uint8_t kPot15Key = 12;
constexpr uint8_t kPot16Semitones = 15;

// Chain order, left to right. Autotune and Octave share one PitchFx engine call, so the
// engine always runs Autotune before Octave whichever slot comes first.
const Slot kSlots[kSlotCount] = {
    {Effect::InputGate, Param::GateThreshold, Param::GateDecay, pins::kToggle1, kPot01, kPot02},
    {Effect::Autotune, Param::AutotuneKey, Param::AutotuneResponse, pins::kToggle2, kPot15Key, kPot03},
    {Effect::Octave, Param::OctaveSemitones, Param::OctaveMix, pins::kToggle3, kPot16Semitones, kPot04},
    {Effect::Unison, Param::UnisonDepth, Param::UnisonRate, pins::kToggle4, kPot05, kPot06},
    {Effect::Slapback, Param::SlapbackMix, Param::SlapbackTime, pins::kToggle5, kPot07, kPot08},
    {Effect::Distortion, Param::DistortionDrive, Param::DistortionTone, pins::kToggle6, kPot09, kPot10},
    {Effect::Gate, Param::GateThreshold, Param::GateDecay, pins::kToggle7, kPot11, kPot12},
    {Effect::Reverb, Param::ReverbMix, Param::ReverbTime, pins::kToggle8, kPot13, kPot14},
};

namespace {

// Input gate live-stage defaults (emulator/main.cpp, owner 2026-10-01).
constexpr float kInputGateThresholdDb = -12.0f;
constexpr float kInputGateRangeDb = -12.0f;
constexpr float kInputGateHoldMs = 100.0f;
constexpr float kInputGateReleaseMs = 10.0f;

constexpr float kGateThresholdMinDb = -70.0f;
constexpr float kGateThresholdMaxDb = -10.0f;
constexpr float kGateDecayMinMs = 5.0f;
constexpr float kGateDecayMaxMs = 1000.0f;
constexpr float kSlapbackTimeMinMs = 60.0f;
constexpr float kSlapbackTimeMaxMs = 250.0f;

// Octave defaults the emulator opens with.
constexpr int kOctaveEngineC = 2;
constexpr float kOctaveStartMix = 0.10f;

// GateParams carries two knobs per stage, so the input gate and the second gate share one setter.
void applyGateParam(Param p, float v, cv::GateParams& g) {
  if (p == Param::GateThreshold) {
    g.thresholdDb = kGateThresholdMinDb + (kGateThresholdMaxDb - kGateThresholdMinDb) * v;
  } else {
    g.tuning.releaseMs = kGateDecayMinMs * powf(kGateDecayMaxMs / kGateDecayMinMs, v);
  }
}

}  // namespace

ChainParams::ChainParams() {
  inputGate.on = true;
  inputGate.thresholdDb = kInputGateThresholdDb;
  inputGate.tuning.rangeDb = kInputGateRangeDb;
  inputGate.tuning.holdMs = kInputGateHoldMs;
  inputGate.tuning.releaseMs = kInputGateReleaseMs;
  gate.on = false;
  pitchFx.autotune.on = false;
  pitchFx.harmony.on = false;  // Harmony is off the face and forced off (owner 2026-10-02)
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

const char* effectName(Effect e) {
  switch (e) {
    case Effect::InputGate: return "Input gate";
    case Effect::Autotune: return "Autotune";
    case Effect::Octave: return "Octave";
    case Effect::Unison: return "Unison";
    case Effect::Slapback: return "Slapback";
    case Effect::Distortion: return "Distortion";
    case Effect::Gate: return "Gate";
    case Effect::Reverb: return "Reverb";
  }
  return "?";
}

const char* paramName(Param p) {
  switch (p) {
    case Param::GateThreshold: return "Threshold";
    case Param::GateDecay: return "Decay";
    case Param::AutotuneKey: return "Key";
    case Param::AutotuneResponse: return "Response";
    case Param::OctaveSemitones: return "Semitones";
    case Param::OctaveMix: return "Mix";
    case Param::UnisonDepth: return "Depth";
    case Param::UnisonRate: return "Rate";
    case Param::SlapbackMix: return "Mix";
    case Param::SlapbackTime: return "Time";
    case Param::DistortionDrive: return "Drive";
    case Param::DistortionTone: return "Tone";
    case Param::ReverbMix: return "Mix";
    case Param::ReverbTime: return "Time";
  }
  return "?";
}

int paramSteps(Param p) {
  if (p == Param::AutotuneKey) return kKeyPositions;
  if (p == Param::OctaveSemitones) return kSemitonePositions;
  return 0;
}

void applyParam(Effect e, Param p, float v, ChainParams& out) {
  switch (p) {
    case Param::GateThreshold:
    case Param::GateDecay:
      applyGateParam(p, v, e == Effect::InputGate ? out.inputGate : out.gate);
      break;
    case Param::AutotuneKey:
      out.pitchFx.autotune.key = static_cast<int>(v);
      break;
    case Param::AutotuneResponse:
      out.pitchFx.autotune.responseMs = cv::AutotuneVoice::responseMsAt(v);
      break;
    case Param::OctaveSemitones:
      out.pitchFx.octave.semitones = kSemitoneMin + static_cast<int>(v);
      break;
    case Param::OctaveMix:
      out.pitchFx.octave.mix = v;
      break;
    case Param::UnisonDepth:
      out.unison.depth = v;
      break;
    case Param::UnisonRate:
      out.unisonRatePercent = 100.0f * v;
      unisonrate::apply(out.unison.tuning, out.unisonRatePercent);
      break;
    case Param::SlapbackMix:
      out.slapback.intensity = v;
      break;
    case Param::SlapbackTime:
      out.slapback.tuning.timeMs =
          kSlapbackTimeMinMs + (kSlapbackTimeMaxMs - kSlapbackTimeMinMs) * v;
      break;
    case Param::DistortionDrive:
      out.distortion.drive = v;
      break;
    case Param::DistortionTone:
      out.distortion.tone = v;
      break;
    case Param::ReverbMix:
      out.reverb.intensity = v;
      cv::applyIntensity(out.reverb);
      break;
    case Param::ReverbTime:
      out.reverb.time = v;
      cv::applyIntensity(out.reverb);
      break;
  }
}

void applyToggle(Effect e, bool on, ChainParams& out) {
  switch (e) {
    case Effect::InputGate: out.inputGate.on = on; break;
    case Effect::Autotune: out.pitchFx.autotune.on = on; break;
    case Effect::Octave: out.pitchFx.octave.on = on; break;
    case Effect::Unison: out.unison.on = on; break;
    case Effect::Slapback: out.slapback.on = on; break;
    case Effect::Distortion: out.distortion.on = on; break;
    case Effect::Gate: out.gate.on = on; break;
    case Effect::Reverb: out.reverb.on = on; break;
  }
}

void formatParam(Param p, float v, char* buf, size_t size) {
  switch (p) {
    case Param::GateThreshold:
      snprintf(buf, size, "%d dB", static_cast<int>(lroundf(kGateThresholdMinDb +
                                   (kGateThresholdMaxDb - kGateThresholdMinDb) * v)));
      break;
    case Param::GateDecay:
      snprintf(buf, size, "%d ms",
               static_cast<int>(lroundf(kGateDecayMinMs * powf(kGateDecayMaxMs / kGateDecayMinMs, v))));
      break;
    case Param::AutotuneKey:
      snprintf(buf, size, "%s", cv::kKeyName[static_cast<int>(v)]);
      break;
    case Param::OctaveSemitones:
      snprintf(buf, size, "%+d st", kSemitoneMin + static_cast<int>(v));
      break;
    case Param::SlapbackTime:
      snprintf(buf, size, "%d ms",
               static_cast<int>(lroundf(kSlapbackTimeMinMs + (kSlapbackTimeMaxMs - kSlapbackTimeMinMs) * v)));
      break;
    default:
      snprintf(buf, size, "%d %%", static_cast<int>(lroundf(100.0f * v)));
      break;
  }
}
