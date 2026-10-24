#include "core/registry.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <array>

#include "core/unison_rate.h"
#include "engine/harmony.h"

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

constexpr std::array<CardDesc, kCardCount> kCards = {{
    {Card::Empty, "empty", "Empty", kNoEngine, 0, {kNoKnob, kNoKnob}, Group::None, costs::kEmpty},
    {Card::InputGate, "input_gate", "Input gate", Engine::InputGate, 2, {kThreshold, kDecay}, Group::None,
     costs::kInputGate},
    {Card::Autotune, "autotune", "Autotune", Engine::Pitch, 2,
     {{"Key", KnobKind::Stepped, kKeyPositions, 0.0f, kKeyPositions - 1, "", Curve::Linear, 0, cv::kKeyName},
      kResponse},
     Group::Pitch, costs::kAutotune},
    {Card::Octave, "octave", "Octave", Engine::Pitch, 2,
     {{"Semitones", KnobKind::Stepped, kSemitonePositions, kSemitoneMin, kSemitoneMax, "st"}, kPercent("Mix")},
     Group::Pitch, costs::kOctave},
    {Card::Unison, "unison", "Unison", Engine::Unison, 2, {kPercent("Depth"), kPercent("Rate")}, Group::None,
     costs::kUnison},
    {Card::Slapback, "slapback", "Slapback", Engine::Slapback, 2, {kPercent("Mix"), kSlapbackTime}, Group::None,
     costs::kSlapback},
    {Card::Distortion, "distortion", "Distortion", Engine::Distortion, 2, {kPercent("Drive"), kPercent("Tone")},
     Group::None, costs::kDistortion},
    {Card::Gate, "gate", "Gate", Engine::Gate, 2, {kThreshold, kDecay}, Group::None, costs::kGate},
    {Card::ReverbSpring, "reverb_spring", "Spring Reverb", Engine::Reverb, 2, {kPercent("Mix"), kPercent("Time")},
     Group::Reverb, costs::kReverbSpring},
    {Card::ReverbChasm, "reverb_chasm", "Chasm Reverb", Engine::Reverb, 2, {kPercent("Mix"), kPercent("Time")},
     Group::Reverb, costs::kReverbChasm},
}};

// Every entry sits at its own enum index with an id that fits the clip width with room to spare,
// so a clipped over-long id can never equal a real one. Empty is the only card without an engine.
constexpr bool tableIsSound() {
  for (int i = 0; i < kCardCount; ++i) {
    const CardDesc& d = kCards[static_cast<size_t>(i)];
    if (static_cast<int>(d.card) != i || d.id == nullptr || d.name == nullptr) return false;
    if (__builtin_strlen(d.id) + 2 > kCardIdBytes) return false;
    if ((d.engine == kNoEngine) != (d.card == Card::Empty)) return false;
  }
  return true;
}
static_assert(tableIsSound(), "kCards must follow the Card enum and keep ids short");

// The position index a stepped value selects, clamped to the knob's positions.
int stepIndex(const KnobDesc& k, float v) {
  const int last = k.positions - 1;
  if (!(v > 0.0f)) return 0;  // also catches NaN
  if (v >= static_cast<float>(last)) return last;
  return static_cast<int>(v);
}

// GateParams carries two knobs per stage, so the input gate and the second gate share one setter.
void applyGateKnob(int knob, float v, cv::GateParams& g) {
  if (knob == 0) {
    g.thresholdDb = knobValue(kThreshold, v);
  } else {
    g.tuning.releaseMs = knobValue(kDecay, v);
  }
}

void applyAutotuneKnob(int knob, float v, cv::AutotuneParams& a) {
  if (knob == 0) {
    a.key = static_cast<int>(v);
  } else {
    a.responseMs = knobValue(kResponse, v);
  }
}

void applyOctaveKnob(int knob, float v, cv::OctaveParams& o) {
  if (knob == 0) {
    o.semitones = kSemitoneMin + static_cast<int>(v);
  } else {
    o.mix = v;
  }
}

void applyUnisonKnob(int knob, float v, cv::UnisonParams& u) {
  if (knob == 0) {
    u.depth = v;
  } else {
    unisonrate::apply(u.tuning, 100.0f * v);
  }
}

void applySlapbackKnob(int knob, float v, cv::SlapbackParams& s) {
  if (knob == 0) {
    s.intensity = v;
  } else {
    s.tuning.timeMs = knobValue(kSlapbackTime, v);
  }
}

void applyDistortionKnob(int knob, float v, cv::DistortionParams& d) {
  if (knob == 0) {
    d.drive = v;
  } else {
    d.tone = v;
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

const CardDesc& cardDesc(Card c) {
  const size_t i = static_cast<size_t>(c);
  return kCards[i < kCards.size() ? i : 0];
}

const KnobDesc* knobDesc(Card c, int knob) {
  const CardDesc& d = cardDesc(c);
  return knob >= 0 && knob < d.knobCount ? &d.knob[knob] : nullptr;
}

Engine engineOf(Card c) { return cardDesc(c).engine; }

bool cardFromId(const char* id, Card& out) {
  if (id == nullptr) return false;
  for (const CardDesc& d : kCards) {
    if (strcmp(id, d.id) == 0) {
      out = d.card;
      return true;
    }
  }
  return false;
}

float knobValue(const KnobDesc& k, float p) {
  switch (k.curve) {
    case Curve::Log: return k.min * powf(k.max / k.min, p);
    case Curve::LogReversed: return k.max * powf(k.min / k.max, p);
    case Curve::Linear: break;
  }
  return k.min + (k.max - k.min) * p;
}

int knobSteps(Card c, int knob) {
  const KnobDesc* k = knobDesc(c, knob);
  return k && k->kind == KnobKind::Stepped ? k->positions : 0;
}

void applyKnob(Card c, int knob, float v, ChainParams& out) {
  const KnobDesc* k = knobDesc(c, knob);
  if (!k) return;
  if (k->kind == KnobKind::Stepped) v = static_cast<float>(stepIndex(*k, v));
  switch (c) {
    case Card::InputGate: applyGateKnob(knob, v, out.inputGate); break;
    case Card::Gate: applyGateKnob(knob, v, out.gate); break;
    case Card::Autotune: applyAutotuneKnob(knob, v, out.pitchFx.autotune); break;
    case Card::Octave: applyOctaveKnob(knob, v, out.pitchFx.octave); break;
    case Card::Unison: applyUnisonKnob(knob, v, out.unison); break;
    case Card::Slapback: applySlapbackKnob(knob, v, out.slapback); break;
    case Card::Distortion: applyDistortionKnob(knob, v, out.distortion); break;
    case Card::ReverbSpring:
    case Card::ReverbChasm:
      applyReverbKnob(knob, v, out.reverb);
      break;
    case Card::Empty:
    case Card::Count: break;
  }
}

void applyToggle(Card c, bool on, ChainParams& out) {
  switch (c) {
    case Card::InputGate: out.inputGate.on = on; break;
    case Card::Autotune: out.pitchFx.autotune.on = on; break;
    case Card::Octave: out.pitchFx.octave.on = on; break;
    case Card::Unison: out.unison.on = on; break;
    case Card::Slapback: out.slapback.on = on; break;
    case Card::Distortion: out.distortion.on = on; break;
    case Card::Gate: out.gate.on = on; break;
    case Card::ReverbSpring:
    case Card::ReverbChasm: out.reverb.on = on; break;
    case Card::Empty:
    case Card::Count: break;
  }
}

void applyPlacement(Card c, ChainParams& out) {
  if (c == Card::ReverbSpring) out.reverb.engine = cv::kReverbSpringB;
  if (c == Card::ReverbChasm) out.reverb.engine = cv::kReverbChasm;
}

int knobShown(const KnobDesc& k, float v) {
  const bool stepped = k.kind == KnobKind::Stepped;
  if (stepped && k.labels) return stepIndex(k, v);
  const float position = v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;  // also catches NaN
  const float value = stepped ? k.min + static_cast<float>(stepIndex(k, v)) : knobValue(k, position);
  return static_cast<int>(lroundf(value * powf(10.0f, static_cast<float>(k.decimals))));
}

const char* knobLabel(const KnobDesc& k, int shown) {
  if (!k.labels) return "";
  const int last = k.positions - 1;
  return k.labels[shown < 0 ? 0 : (shown > last ? last : shown)];
}

void formatShown(const KnobDesc& k, int shown, char* buf, size_t size) {
  if (size == 0) return;
  if (k.labels) {
    snprintf(buf, size, "%s", knobLabel(k, shown));
    return;
  }
  const char* sign = shown < 0 ? "-" : (shown > 0 && k.min < 0.0f ? "+" : "");
  if (k.decimals == 0) {
    snprintf(buf, size, "%s%d", sign, shown < 0 ? -shown : shown);
    return;
  }
  const int scale = static_cast<int>(lroundf(powf(10.0f, static_cast<float>(k.decimals))));
  const int magnitude = shown < 0 ? -shown : shown;
  snprintf(buf, size, "%s%d.%0*d", sign, magnitude / scale, static_cast<int>(k.decimals), magnitude % scale);
}

void formatKnob(Card c, int knob, float v, char* buf, size_t size) {
  if (size == 0) return;
  buf[0] = '\0';
  const KnobDesc* k = knobDesc(c, knob);
  if (!k) return;
  char shown[16];
  formatShown(*k, knobShown(*k, v), shown, sizeof(shown));
  if (k->labels || k->unit[0] == '\0') {
    snprintf(buf, size, "%s", shown);
  } else {
    snprintf(buf, size, "%s %s", shown, k->unit);
  }
}
