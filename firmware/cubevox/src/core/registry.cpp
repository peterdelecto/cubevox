#include "core/registry.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <array>

#include "core/unison_rate.h"
#include "engine/scale.h"

namespace {

constexpr int kKeyPositions = cv::kKeyCount;
constexpr int kSemitoneMin = -cv::OctaveVoice::kMaxSemis;
constexpr int kSemitoneMax = cv::OctaveVoice::kMaxSemis;
constexpr int kSemitonePositions = kSemitoneMax - kSemitoneMin + 1;
static_assert(kSemitonePositions == 25, "SEMITONES has 25 positions, -12 to +12");
constexpr int kFormantShiftMin = -cv::kFormantMaxShiftSt;
constexpr int kFormantShiftMax = cv::kFormantMaxShiftSt;
constexpr int kFormantShiftPositions = kFormantShiftMax - kFormantShiftMin + 1;

constexpr KnobDesc kNoKnob = {"", KnobKind::Continuous, 0, 0.0f, 0.0f, ""};
constexpr KnobDesc kPercent(const char* name) { return {name, KnobKind::Continuous, 0, 0.0f, 100.0f, "%"}; }
constexpr KnobDesc kThreshold = {"Threshold", KnobKind::Continuous, 0, cv::kGateThresholdMinDb, cv::kGateThresholdMaxDb,
                                 "dB"};
constexpr KnobDesc kInputThreshold = {"Threshold", KnobKind::Continuous, 0, cv::kInputGateThresholdMinDb,
                                      cv::kInputGateThresholdMaxDb, "dB"};
constexpr KnobDesc kInputDecay = {"Decay", KnobKind::Continuous, 0, cv::kInputGateDecayMinMs,
                                  cv::kInputGateDecayMaxMs, "ms", Curve::Log};
constexpr KnobDesc kSlapbackTime = {"Time", KnobKind::Continuous, 0, cv::Slapback::kTimeMinMs,
                                    cv::Slapback::kTimeMaxMs, "ms"};
constexpr KnobDesc kDecay = {"Decay", KnobKind::Continuous, 0, cv::kGateDecayMinMs, cv::kGateDecayMaxMs, "ms",
                             Curve::Log};
constexpr KnobDesc kResponse = {"Response", KnobKind::Continuous, 0, cv::AutotuneVoice::kMinResponseMs,
                                cv::AutotuneVoice::kMaxResponseMs, "ms", Curve::LogReversed};
constexpr KnobDesc kFormantShift = {"SHIFT", KnobKind::Stepped, kFormantShiftPositions, kFormantShiftMin,
                                    kFormantShiftMax, "st"};
constexpr KnobDesc kFollowerRange = {"RANGE", KnobKind::Continuous, 0, cv::kFollowerMinRangeSt,
                                     cv::kFollowerMaxRangeSt, "st", Curve::ZeroAtNoon};
constexpr KnobDesc kBloomLength = {"LENGTH", KnobKind::Continuous, 0, cv::Bloom::kLengthMinMs, cv::Bloom::kLengthMaxMs, "ms",
                                 Curve::Log};

// Knobs are listed in knob order, A then B. Knob A is the upper pot of the slot and knob B the lower.
// A card with an amount knob (Mix, Amount, Depth, Drive) puts it on knob B. The apply functions below
// take the knob index in this order.
constexpr std::array<CardDesc, kCardCount> kCards = {{
    {Card::Empty, "empty", "Empty", kNoEngine, 0, {kNoKnob, kNoKnob}, Group::None, costs::kEmpty},
    {Card::InputGate, "input_gate", "Input gate", Engine::InputGate, 2, {kInputThreshold, kInputDecay}, Group::None,
     costs::kInputGate},
    {Card::Autotune, "autotune", "Autotune", Engine::Pitch, 2,
     {{"Key", KnobKind::Stepped, kKeyPositions, 0.0f, kKeyPositions - 1, "", Curve::Linear, 0, cv::kKeyName.data()},
      kResponse},
     Group::Pitch, costs::kAutotune},
    {Card::Octave, "octave", "Octave", Engine::Pitch, 2,
     {{"Semitones", KnobKind::Stepped, kSemitonePositions, kSemitoneMin, kSemitoneMax, "st"}, kPercent("Mix")},
     Group::Pitch, costs::kOctave},
    {Card::Unison, "unison", "Unison", Engine::Unison, 2, {kPercent("Rate"), kPercent("Depth")}, Group::None,
     costs::kUnison},
    {Card::Slapback, "slapback", "Echo", Engine::Slapback, 2, {kSlapbackTime, kPercent("AMOUNT")}, Group::None,
     costs::kSlapback},
    {Card::Distortion, "distortion", "Distortion", Engine::Distortion, 2, {kPercent("Tone"), kPercent("Drive")},
     Group::None, costs::kDistortion},
    {Card::Gate, "gate", "Gate", Engine::Gate, 2, {kThreshold, kDecay}, Group::None, costs::kGate},
    {Card::ReverbSpring, "reverb_spring", "Spring Reverb", Engine::Reverb, 2, {kPercent("Time"), kPercent("Mix")},
     Group::Reverb, costs::kReverbSpring},
    {Card::ReverbChasm, "reverb_chasm", "Chasm Reverb", Engine::Reverb, 2, {kPercent("Time"), kPercent("Mix")},
     Group::Reverb, costs::kReverbChasm},
    {Card::Crowd, "crowd", "Mob", Engine::Crowd, 2,
     {kPercent("DIVERGE"),
      {"VOICES", KnobKind::Stepped, cv::kVoicesSteps, 0.0f, cv::kVoicesSteps - 1, "", Curve::Linear, 0,
       cv::kVoicesName.data()}},
     Group::Pitch, costs::kCrowd},
    {Card::Formant, "formant", "Formant", Engine::Pitch, 2, {kFormantShift, kPercent("MIX")}, Group::Pitch,
     costs::kFormant},
    {Card::Lofi, "lofi", "Lo-fi", Engine::Lofi, 2, {kPercent("TONE"), kPercent("AMOUNT")}, Group::None, costs::kLofi},
    {Card::Bloom, "bloom", "Chatter", Engine::Bloom, 2, {kBloomLength, kPercent("AMOUNT")}, Group::None, costs::kBloom},
    {Card::FormantFollower, "formant_follow", "Yawn", Engine::Pitch, 2, {kPercent("SENSITIVITY"), kFollowerRange},
     Group::Pitch, costs::kFormantFollower},
    {Card::Growl, "growl", "Growl", Engine::Growl, 2, {kPercent("GROWL"), kPercent("DRIVE")}, Group::Pitch, costs::kGrowl},
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

// GateParams carries two knobs per stage, so the Input gate and the Gate share one setter and bring
// their own ranges.
void applyGateKnob(int knob, float v, cv::GateParams& g, const KnobDesc& threshold, const KnobDesc& decay) {
  if (knob == 0) {
    g.thresholdDb = knobValue(threshold, v);
  } else {
    g.tuning.releaseMs = knobValue(decay, v);
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
    unisonrate::apply(u.tuning, 100.0f * v);
  } else {
    u.depth = v;
  }
}

void applySlapbackKnob(int knob, float v, cv::SlapbackParams& s) {
  if (knob == 0) {
    s.tuning.timeMs = knobValue(kSlapbackTime, v);
  } else {
    s.amount = v;
  }
}

void applyDistortionKnob(int knob, float v, cv::DistortionParams& d) {
  if (knob == 0) {
    d.tone = v;
  } else {
    d.drive = v;
  }
}

void applyCrowdKnob(int knob, float v, cv::CrowdParams& d) {
  if (knob == 0) {
    d.width = v;
  } else {
    d.voicesStep = static_cast<uint8_t>(v);
  }
}

void applyFormantKnob(int knob, float v, cv::FormantParams& f) {
  if (knob == 0) {
    f.shiftSt = kFormantShiftMin + static_cast<int>(v);
  } else {
    f.mix = v;
  }
}

// Yawn's SENSITIVITY sets the level that gives the full sweep and RANGE the sweep's reach, -12 st at the
// pot's bottom through 0 at noon to +6 st at its top. The card pins the Formant voice's mix at 1, so no
// knob sets it.
void applyFollowerKnob(int knob, float v, ChainParams& out) {
  if (knob == 0) {
    out.follower.sensitivity = v;
  } else {
    out.follower.rangeSt = knobValue(kFollowerRange, v);
  }
}

void applyLofiKnob(int knob, float v, cv::LofiParams& l) {
  if (knob == 0) {
    l.tone = v;
  } else {
    l.amount = v * kLofiAmountTop;
  }
}

void applyBloomKnob(int knob, float v, cv::BloomParams& b) {
  if (knob == 0) {
    b.lengthMs = knobValue(kBloomLength, v);
  } else {
    b.amount = v;
  }
}

void applyGrowlKnob(int knob, float v, cv::GrowlParams& g) {
  if (knob == 0) {
    g.growl = v;
  } else {
    g.drive = v;
  }
}

void applyReverbKnob(int knob, float v, cv::ReverbParams& r) {
  if (knob == 0) {
    r.time = v;
  } else {
    r.mix = v;
  }
  cv::applyKnobs(r);
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

const char* oledName(Card c) {
  const CardDesc& d = cardDesc(c);
  return d.oledName != nullptr ? d.oledName : d.name;
}

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
    case Curve::Log: return cv::logTaper(k.min, k.max, p);
    case Curve::LogReversed: return cv::logTaper(k.max, k.min, p);
    case Curve::ZeroAtNoon: return p < 0.5f ? k.min * (1.0f - 2.0f * p) : k.max * (2.0f * p - 1.0f);
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
    case Card::InputGate: applyGateKnob(knob, v, out.inputGate, kInputThreshold, kInputDecay); break;
    case Card::Gate: applyGateKnob(knob, v, out.gate, kThreshold, kDecay); break;
    case Card::Autotune: applyAutotuneKnob(knob, v, out.pitchFx.autotune); break;
    case Card::Octave: applyOctaveKnob(knob, v, out.pitchFx.octave); break;
    case Card::Unison: applyUnisonKnob(knob, v, out.unison); break;
    case Card::Slapback: applySlapbackKnob(knob, v, out.slapback); break;
    case Card::Distortion: applyDistortionKnob(knob, v, out.distortion); break;
    case Card::ReverbSpring:
    case Card::ReverbChasm:
      applyReverbKnob(knob, v, out.reverb);
      break;
    case Card::Crowd: applyCrowdKnob(knob, v, out.crowd); break;
    case Card::Formant: applyFormantKnob(knob, v, out.pitchFx.formant); break;
    case Card::Lofi: applyLofiKnob(knob, v, out.lofi); break;
    case Card::Bloom: applyBloomKnob(knob, v, out.bloom); break;
    case Card::FormantFollower: applyFollowerKnob(knob, v, out); break;
    case Card::Growl: applyGrowlKnob(knob, v, out.growl); break;
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
    case Card::Crowd:
      // The pitch stage reads crowdOn to keep its tracker and ring running for the Crowd card.
      out.crowd.on = on;
      out.pitchFx.crowdOn = on;
      break;
    case Card::Formant: out.pitchFx.formant.on = on; break;
    case Card::Lofi: out.lofi.on = on; break;
    case Card::Bloom: out.bloom.on = on; break;
    case Card::FormantFollower: out.pitchFx.formant.on = on; break;
    case Card::Growl:
      // The pitch stage reads growlOn to keep its tracker and ring running for the Growl card.
      out.growl.on = on;
      out.pitchFx.growlOn = on;
      break;
    case Card::Empty:
    case Card::Count: break;
  }
}

void applyPlacement(Card c, ChainParams& out) {
  if (c == Card::ReverbSpring) out.reverb.engine = cv::kReverbSpring;
  if (c == Card::ReverbChasm) out.reverb.engine = cv::kReverbChasm;
  // The Chatter card takes its word key from the Input gate's detector whenever that card is placed.
  if (c == Card::InputGate) out.bloom.keyFromGate = true;
  // The Formant follower's envelope sets the Formant voice's SHIFT, through the toggle's fade too.
  if (c == Card::FormantFollower) out.follower.placed = true;
}

namespace {

// 10 to the power n, for the few decimals a knob declares.
unsigned tenPow(unsigned n) {
  unsigned p = 1;
  for (; n > 0; --n) p *= 10;
  return p;
}

unsigned magnitudeOf(int shown) {
  return shown < 0 ? 0u - static_cast<unsigned>(shown) : static_cast<unsigned>(shown);
}

// Writes a magnitude as text, its digits with the point and decimal digits when the knob declares
// decimals, and returns the length written.
int magnitudeText(const KnobDesc& k, unsigned magnitude, char* out, size_t size) {
  const unsigned scale = tenPow(k.decimals);
  int length;
  if (k.decimals == 0) {
    length = snprintf(out, size, "%u", magnitude);
  } else {
    length = snprintf(out, size, "%u.%0*u", magnitude / scale, static_cast<int>(k.decimals), magnitude % scale);
  }
  return length < static_cast<int>(size) ? length : static_cast<int>(size) - 1;
}

}  // namespace

int knobShown(const KnobDesc& k, float v) {
  const bool stepped = k.kind == KnobKind::Stepped;
  if (stepped && k.labels) return stepIndex(k, v);
  const float position = v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;  // also catches NaN
  const float value = stepped ? k.min + static_cast<float>(stepIndex(k, v)) : knobValue(k, position);
  return static_cast<int>(lroundf(value * static_cast<float>(tenPow(k.decimals))));
}

const char* knobLabel(const KnobDesc& k, int shown) {
  if (!k.labels) return "";
  const int last = k.positions - 1;
  return k.labels[shown < 0 ? 0 : (shown > last ? last : shown)];
}

int knobCells(const KnobDesc& k) {
  if (k.labels) return 0;
  const float scale = static_cast<float>(tenPow(k.decimals));
  char text[16];
  const int low = magnitudeText(k, magnitudeOf(static_cast<int>(lroundf(k.min * scale))), text, sizeof(text));
  const int high = magnitudeText(k, magnitudeOf(static_cast<int>(lroundf(k.max * scale))), text, sizeof(text));
  const int cells = (k.min < 0.0f ? 1 : 0) + (low > high ? low : high);
  return cells < kMaxKnobCells ? cells : kMaxKnobCells;
}

void formatCells(const KnobDesc& k, int shown, char* buf, size_t size) {
  if (size == 0) return;
  const int cells = knobCells(k);
  buf[0] = '\0';
  if (cells == 0 || size <= static_cast<size_t>(cells)) return;

  const bool signedRange = k.min < 0.0f;
  char text[16];
  const int length = magnitudeText(k, magnitudeOf(shown), text, sizeof(text));
  const int digitCells = cells - (signedRange ? 1 : 0);
  const int kept = length < digitCells ? length : digitCells;
  memset(buf, ' ', static_cast<size_t>(cells));
  if (signedRange) buf[0] = shown < 0 ? '-' : (shown > 0 ? '+' : ' ');
  memcpy(buf + cells - kept, text + length - kept, static_cast<size_t>(kept));
  buf[cells] = '\0';
}
