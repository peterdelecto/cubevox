#include "core/registry.h"

#include <math.h>
#include <stdio.h>

#include "core/costs.h"
#include "engine/harmony.h"
#include "core/unison_rate.h"

namespace {

constexpr float kGateThresholdMinDb = -70.0f;
constexpr float kGateThresholdMaxDb = -10.0f;
constexpr float kGateDecayMinMs = 5.0f;
constexpr float kGateDecayMaxMs = 1000.0f;
constexpr float kSlapbackTimeMinMs = 60.0f;
constexpr float kSlapbackTimeMaxMs = 250.0f;
constexpr int kKeyPositions = sizeof(cv::kKeyName) / sizeof(cv::kKeyName[0]);
constexpr int kSemitoneMin = -12;
constexpr int kSemitoneMax = 12;
constexpr int kSemitonePositions = kSemitoneMax - kSemitoneMin + 1;

constexpr KnobDesc kNoKnob = {"", KnobKind::Continuous, 0, 0.0f, 0.0f, ""};
constexpr KnobDesc kPercent(const char* name) { return {name, KnobKind::Continuous, 0, 0.0f, 100.0f, "%"}; }
constexpr KnobDesc kThreshold = {"Threshold", KnobKind::Continuous, 0, kGateThresholdMinDb, kGateThresholdMaxDb, "dB"};
constexpr KnobDesc kSlapbackTime = {"Time", KnobKind::Continuous, 0, kSlapbackTimeMinMs, kSlapbackTimeMaxMs, "ms"};
constexpr KnobDesc kDecay = {"Decay", KnobKind::Continuous, 0, kGateDecayMinMs, kGateDecayMaxMs, "ms", Curve::Log};
constexpr KnobDesc kResponse = {"Response", KnobKind::Continuous, 0, cv::AutotuneVoice::kMinResponseMs,
                                cv::AutotuneVoice::kMaxResponseMs, "ms", Curve::LogReversed};

const CardDesc kCards[kCardCount] = {
    {"empty", "Empty", 0, {kNoKnob, kNoKnob}, Group::None, costs::kEmpty, true},
    {"input_gate", "Input gate", 2, {kThreshold, kDecay}, Group::None, costs::kInputGate, true},
    {"autotune", "Autotune", 2,
     {{"Key", KnobKind::Stepped, kKeyPositions, 0.0f, kKeyPositions - 1, ""},
      kResponse},
     Group::Pitch, costs::kAutotune, true},
    {"octave", "Octave", 2,
     {{"Semitones", KnobKind::Stepped, kSemitonePositions, kSemitoneMin, kSemitoneMax, "st"}, kPercent("Mix")},
     Group::Pitch, costs::kOctave, true},
    {"unison", "Unison", 2, {kPercent("Depth"), kPercent("Rate")}, Group::None, costs::kUnison, true},
    {"slapback", "Slapback", 2,
     {kPercent("Mix"), kSlapbackTime},
     Group::None, costs::kSlapback, true},
    {"distortion", "Distortion", 2, {kPercent("Drive"), kPercent("Tone")}, Group::None, costs::kDistortion, true},
    {"gate", "Gate", 2, {kThreshold, kDecay}, Group::None, costs::kGate, true},
    {"reverb_spring", "Spring Reverb", 2, {kPercent("Mix"), kPercent("Time")}, Group::Reverb, costs::kReverbSpring, true},
    {"reverb_chasm", "Chasm Reverb", 2, {kPercent("Mix"), kPercent("Time")}, Group::Reverb, costs::kReverbChasm, true},
};

// GateParams carries two knobs per stage, so the input gate and the second gate share one setter.
void applyGateKnob(int knob, float v, cv::GateParams& g) {
  if (knob == 0) {
    g.thresholdDb = knobValue(kThreshold, v);
  } else {
    g.tuning.releaseMs = knobValue(kDecay, v);
  }
}

void applyReverbKnob(int knob, float v, cv::ReverbParams& r) {
  if (knob == 0) {
    r.intensity = v;
  } else {
    r.time = v;
  }
  cv::applyIntensity(r);
}

}  // namespace

const CardDesc& cardDesc(Card c) { return kCards[static_cast<uint8_t>(c)]; }

float knobValue(const KnobDesc& k, float p) {
  switch (k.curve) {
    case Curve::Log: return k.min * powf(k.max / k.min, p);
    case Curve::LogReversed: return k.max * powf(k.min / k.max, p);
    case Curve::Linear: break;
  }
  return k.min + (k.max - k.min) * p;
}

int knobSteps(Card c, int knob) {
  const KnobDesc& k = cardDesc(c).knob[knob];
  return k.kind == KnobKind::Stepped ? k.positions : 0;
}

void applyKnob(Card c, int knob, float v, ChainParams& out) {
  switch (c) {
    case Card::Empty: break;
    case Card::InputGate: applyGateKnob(knob, v, out.inputGate); break;
    case Card::Gate: applyGateKnob(knob, v, out.gate); break;
    case Card::Autotune:
      if (knob == 0) {
        out.pitchFx.autotune.key = static_cast<int>(v);
      } else {
        out.pitchFx.autotune.responseMs = knobValue(kResponse, v);
      }
      break;
    case Card::Octave:
      if (knob == 0) {
        out.pitchFx.octave.semitones = kSemitoneMin + static_cast<int>(v);
      } else {
        out.pitchFx.octave.mix = v;
      }
      break;
    case Card::Unison:
      if (knob == 0) {
        out.unison.depth = v;
      } else {
        out.unisonRatePercent = 100.0f * v;
        unisonrate::apply(out.unison.tuning, out.unisonRatePercent);
      }
      break;
    case Card::Slapback:
      if (knob == 0) {
        out.slapback.intensity = v;
      } else {
        out.slapback.tuning.timeMs = knobValue(kSlapbackTime, v);
      }
      break;
    case Card::Distortion:
      if (knob == 0) {
        out.distortion.drive = v;
      } else {
        out.distortion.tone = v;
      }
      break;
    case Card::ReverbSpring:
    case Card::ReverbChasm:
      applyReverbKnob(knob, v, out.reverb);
      break;
  }
}

void applyToggle(Card c, bool on, ChainParams& out) {
  switch (c) {
    case Card::Empty: break;
    case Card::InputGate: out.inputGate.on = on; break;
    case Card::Autotune: out.pitchFx.autotune.on = on; break;
    case Card::Octave: out.pitchFx.octave.on = on; break;
    case Card::Unison: out.unison.on = on; break;
    case Card::Slapback: out.slapback.on = on; break;
    case Card::Distortion: out.distortion.on = on; break;
    case Card::Gate: out.gate.on = on; break;
    case Card::ReverbSpring:
    case Card::ReverbChasm: out.reverb.on = on; break;
  }
}

void applyPlacement(Card c, ChainParams& out) {
  if (c == Card::ReverbSpring) out.reverb.engine = cv::kReverbSpringB;
  if (c == Card::ReverbChasm) out.reverb.engine = cv::kReverbChasm;
}

void formatKnob(Card c, int knob, float v, char* buf, size_t size) {
  const KnobDesc& k = cardDesc(c).knob[knob];
  if (c == Card::Autotune && knob == 0) {
    snprintf(buf, size, "%s", cv::kKeyName[static_cast<int>(v)]);
    return;
  }
  if (k.kind == KnobKind::Stepped) {
    snprintf(buf, size, "%+d %s", static_cast<int>(k.min) + static_cast<int>(v), k.unit);
    return;
  }
  snprintf(buf, size, "%d %s", static_cast<int>(lroundf(knobValue(k, v))), k.unit);
}
