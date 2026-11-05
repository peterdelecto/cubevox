// Checks the card registry on the host. Covers ids, descriptors, knob mapping, clamps, chain
// defaults and readouts.

#include "support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/canvas.h"
#include "core/chain_params.h"
#include "core/fonts.h"
#include "core/layout_fade.h"
#include "core/registry.h"
#include "core/unison_rate.h"

namespace {

bool snakeCase(const char* s) {
  if (!*s) return false;
  for (; *s; ++s) {
    const bool ok = (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_';
    if (!ok) return false;
  }
  return true;
}

void checkIds() {
  for (int i = 0; i < kCardCount; ++i) {
    const CardDesc& d = cardDesc(card(i));
    check(snakeCase(d.id), d.id);
    check(d.name[0] != '\0', "display name present");
    for (int j = 0; j < kCardCount; ++j) {
      if (i != j) check(std::strcmp(d.id, cardDesc(card(j)).id) != 0, "ids unique");
    }
    check(d.knobCount == (card(i) == Card::Empty ? 0 : kKnobsPerCard), "knob count");
    for (int k = 0; k < d.knobCount; ++k) {
      const KnobDesc& kd = d.knob[k];
      check(kd.name[0] != '\0', "knob name present");
      check((kd.kind == KnobKind::Stepped) == (kd.positions > 0), "positions only on stepped knobs");
      check(knobSteps(card(i), k) == (kd.kind == KnobKind::Stepped ? kd.positions : 0), "knobSteps matches kind");
    }
  }
  check(std::strcmp(cardDesc(Card::ReverbSpring).name, "Spring Reverb") == 0, "spring reverb name");
  check(std::strcmp(cardDesc(Card::ReverbChasm).name, "Chasm Reverb") == 0, "chasm reverb name");
}

void checkCardTable() {
  check(kCardCount == static_cast<int>(Card::Count), "table: the count follows the enum");
  for (int i = 0; i < kCardCount; ++i) {
    const CardDesc& d = cardDesc(card(i));
    check(d.card == card(i), "table: an entry sits at its enum index");
    check(std::strlen(d.id) + 2 <= kCardIdBytes, "table: an id leaves room under the clip width");
    check((engineOf(card(i)) == kNoEngine) == (card(i) == Card::Empty), "table: only Empty has no engine");
    Card back = Card::Empty;
    check(cardFromId(d.id, back) && back == card(i), d.id);
  }
  Card out = Card::Gate;
  check(!cardFromId("nope", out) && !cardFromId("", out) && !cardFromId(nullptr, out) && out == Card::Gate,
        "table: an unknown id finds nothing and leaves the output alone");
  check(&cardDesc(Card::Count) == &cardDesc(Card::Empty) && &cardDesc(card(200)) == &cardDesc(Card::Empty),
        "table: an out-of-range card reads as Empty");
  check(engineOf(Card::Autotune) == Engine::Autotune && engineOf(Card::Octave) == Engine::Octave &&
            engineOf(Card::Formant) == Engine::Formant && engineOf(Card::FormantFollower) == Engine::Formant,
        "table: Autotune and Octave have an engine each, and Formant and Apraxia share the Formant engine");
  check(engineOf(Card::ReverbSpring) == Engine::Reverb && engineOf(Card::ReverbChasm) == Engine::Reverb,
        "table: both reverb cards share the reverb engine");
  check(engineOf(Card::Crowd) == Engine::Crowd && cardDesc(Card::Crowd).group == Group::Pitch,
        "table: the Crowd card has its own engine and sits in the pitch group");
  check(std::strcmp(cardDesc(Card::Crowd).id, "crowd") == 0 && std::strcmp(cardDesc(Card::Crowd).name, "Mob") == 0 &&
            std::strcmp(knobDesc(Card::Crowd, 0)->name, "DIVERGE") == 0 &&
            std::strcmp(knobDesc(Card::Crowd, 1)->name, "VOICES") == 0,
        "table: the Crowd card reads DIVERGE then VOICES");
  check(kFadeHoldBlocks == kEngineCount - 1, "table: one silent block per engine after the swap block");
}

void checkGroups() {
  int reverbs = 0;
  int pitch = 0;
  for (int i = 0; i < kCardCount; ++i) {
    const Group g = cardDesc(card(i)).group;
    if (g == Group::Reverb) ++reverbs;
    if (g == Group::Pitch) ++pitch;
  }
  check(reverbs == 2, "two reverb-group cards: Spring and Chasm");
  check(pitch == 6 && cardDesc(Card::FormantFollower).group == Group::Pitch,
        "autotune, octave, formant, Apraxia, the crowd card and growl are the pitch group");
}

// The amount knob (Mix, Amount, Depth, Drive, Range) sits on knob B, the lower pot of its slot, and each parameter
// still reaches its own engine field through the knob it is listed on.
void checkAmountKnobIsB() {
  const Card amountCards[] = {Card::Unison, Card::Slapback, Card::Distortion, Card::ReverbSpring, Card::ReverbChasm,
                              Card::Crowd,  Card::Formant,  Card::Lofi,       Card::Bloom,       Card::FormantFollower,
                              Card::Growl};
  const auto isAmount = [](const char* name) {
    return std::strcmp(name, "Mix") == 0 || std::strcmp(name, "MIX") == 0 || std::strcmp(name, "AMOUNT") == 0 ||
           std::strcmp(name, "Depth") == 0 || std::strcmp(name, "Drive") == 0 || std::strcmp(name, "DRIVE") == 0 ||
           std::strcmp(name, "VOICES") == 0 || std::strcmp(name, "RANGE") == 0;
  };
  for (const Card c : amountCards) {
    char what[96];
    std::snprintf(what, sizeof(what), "%s: knob B is the amount knob, not %s", cardDesc(c).id, knobDesc(c, 0)->name);
    check(isAmount(knobDesc(c, 1)->name), what);
    std::snprintf(what, sizeof(what), "%s: knob A is not an amount knob", cardDesc(c).id);
    check(!isAmount(knobDesc(c, 0)->name), what);
  }
  ChainParams p;
  applyKnob(Card::Unison, 1, 0.7f, p);
  check(std::fabs(p.unison.depth - 0.7f) < 1e-6f, "amount: Unison knob B drives Depth");
  applyKnob(Card::Slapback, 1, 0.7f, p);
  check(std::fabs(p.slapback.amount - 0.7f) < 1e-6f, "amount: Echo knob B drives the wet level and repeats");
  check(std::strcmp(knobDesc(Card::Slapback, 1)->name, "AMOUNT") == 0, "amount: Slapback knob B is named AMOUNT");
  applyKnob(Card::Distortion, 1, 0.7f, p);
  check(std::fabs(p.distortion.drive - 0.7f) < 1e-6f, "amount: Distortion knob B drives Drive");
  applyKnob(Card::ReverbSpring, 1, 0.7f, p);
  check(std::fabs(p.reverb.mix - 0.7f) < 1e-6f, "amount: Spring Reverb knob B drives Mix");
  applyKnob(Card::ReverbChasm, 1, 0.4f, p);
  check(std::fabs(p.reverb.mix - 0.4f) < 1e-6f, "amount: Chasm Reverb knob B drives Mix");
  applyKnob(Card::Crowd, 1, 7.0f, p);
  check(p.crowd.voicesStep == 7 && std::fabs(p.crowd.width - cv::CrowdParams().width) < 1e-6f,
        "amount: Crowd knob B drives the VOICES step and leaves DIVERGE alone");
  applyKnob(Card::Crowd, 0, 0.3f, p);
  check(std::fabs(p.crowd.width - 0.3f) < 1e-6f && p.crowd.voicesStep == 7,
        "amount: Crowd knob A drives DIVERGE and leaves VOICES alone");
  check(std::strcmp(knobDesc(Card::Octave, 1)->name, "Mix") == 0, "amount: Octave keeps Mix on knob B");
}

// Every knob at 0 and at 1 must land on a different engine value.
void checkKnobsMove() {
  for (int i = 0; i < kCardCount; ++i) {
    const Card c = card(i);
    for (int k = 0; k < cardDesc(c).knobCount; ++k) {
      ChainParams lo;
      ChainParams hi;
      const float top = knobSteps(c, k) > 0 ? static_cast<float>(knobSteps(c, k) - 1) : 1.0f;
      applyKnob(c, k, 0.0f, lo);
      applyKnob(c, k, top, hi);
      char what[64];
      std::snprintf(what, sizeof(what), "%s knob %d moves", cardDesc(c).id, k);
      check(std::memcmp(&lo, &hi, sizeof(ChainParams)) != 0, what);
    }
  }
}

// The engine field a continuous knob drives, in the field's own unit (percent knobs hold 0..1).
float engineValue(Card c, int knob, const ChainParams& p) {
  switch (c) {
    case Card::InputGate: return knob == 0 ? p.inputGate.thresholdDb : p.inputGate.tuning.releaseMs;
    case Card::Gate: return knob == 0 ? p.gate.thresholdDb : p.gate.tuning.releaseMs;
    case Card::Autotune: return p.pitchFx.autotune.responseMs;
    case Card::Octave: return p.pitchFx.octave.mix;
    case Card::Unison: return knob == 0 ? p.unison.tuning.lfoHz[0] : p.unison.depth;
    case Card::Slapback: return knob == 0 ? p.slapback.tuning.timeMs : p.slapback.amount;
    case Card::Distortion: return knob == 0 ? p.distortion.tone : p.distortion.drive;
    case Card::ReverbSpring:
    case Card::ReverbChasm: return knob == 0 ? p.reverb.time : p.reverb.mix;
    case Card::Crowd: return knob == 0 ? p.crowd.width : static_cast<float>(p.crowd.voicesStep);
    case Card::Formant: return knob == 0 ? static_cast<float>(p.pitchFx.formant.shiftSt) : p.pitchFx.formant.mix;
    case Card::Lofi: return knob == 0 ? p.lofi.tone : p.lofi.amount;
    case Card::Bloom: return knob == 0 ? p.bloom.lengthMs : p.bloom.amount;
    case Card::FormantFollower: return knob == 0 ? p.follower.sensitivity : p.follower.rangeSt;
    case Card::Growl: return knob == 0 ? p.growl.growl : p.growl.mix;
    case Card::Empty:
    case Card::Count: break;
  }
  return 0.0f;
}

// Descriptor, the OLED number and applyKnob must tell the same story for every continuous knob.
void checkDescriptors() {
  for (int i = 1; i < kCardCount; ++i) {
    const Card c = card(i);
    for (int k = 0; k < cardDesc(c).knobCount; ++k) {
      const KnobDesc& d = cardDesc(c).knob[k];
      if (d.kind != KnobKind::Continuous) continue;
      char what[96];
      check(d.min < d.max, "descriptor min below max");
      const bool reversed = d.curve == Curve::LogReversed;
      std::snprintf(what, sizeof(what), "%s knob %d shows its ends at 0 and 1", cardDesc(c).id, k);
      const float shownScale = std::pow(10.0f, static_cast<float>(d.decimals));
      check(knobShown(d, 0.0f) == std::lround((reversed ? d.max : d.min) * shownScale) &&
                knobShown(d, 1.0f) == std::lround((reversed ? d.min : d.max) * shownScale),
            what);

      const bool percent = std::strcmp(d.unit, "%") == 0;
      float prev = 0.0f;
      for (int s = 0; s <= 4; ++s) {
        ChainParams p;
        applyKnob(c, k, static_cast<float>(s) / 4.0f, p);
        const float v = engineValue(c, k, p);
        std::snprintf(what, sizeof(what), "%s knob %d engine value %d", cardDesc(c).id, k, s);
        if (s > 0) check(reversed ? v < prev : v > prev, what);
        if (!percent) check(std::fabs(v - knobValue(d, static_cast<float>(s) / 4.0f)) < 1e-3f, what);
        prev = v;
      }
    }
  }
  check(std::fabs(knobValue(cardDesc(Card::Autotune).knob[1], 0.3f) - cv::AutotuneVoice::responseMsAt(0.3f)) < 1e-3f,
        "response curve matches the engine");
  check(knobSteps(Card::Autotune, 0) == cv::kKeyCount, "key positions equal cv::kKeyCount");
}

void checkKnobTargets() {
  ChainParams p;
  applyKnob(Card::InputGate, 0, 1.0f, p);
  check(std::fabs(p.inputGate.thresholdDb - (-30.0f)) < 1e-4f, "input gate threshold max -30 dB");
  applyKnob(Card::InputGate, 0, 0.0f, p);
  check(std::fabs(p.inputGate.thresholdDb - (-80.0f)) < 1e-4f, "input gate threshold min -80 dB");
  applyKnob(Card::InputGate, 1, 0.0f, p);
  check(std::fabs(p.inputGate.tuning.releaseMs - 20.0f) < 1e-3f, "input gate decay min 20 ms");
  applyKnob(Card::InputGate, 1, 1.0f, p);
  check(std::fabs(p.inputGate.tuning.releaseMs - 2000.0f) < 1e-1f, "input gate decay max 2000 ms");
  check(knobShown(*knobDesc(Card::InputGate, 0), 0.5f) == -55 && knobShown(*knobDesc(Card::InputGate, 0), 0.02f) == -79,
        "input gate threshold shows in whole dB");
  applyKnob(Card::Gate, 0, 0.0f, p);
  check(std::fabs(p.gate.thresholdDb - (-70.0f)) < 1e-4f, "gate threshold min -70 dB");
  applyKnob(Card::Gate, 1, 1.0f, p);
  check(std::fabs(p.gate.tuning.releaseMs - 1000.0f) < 1e-2f, "gate decay max 1000 ms");
  applyKnob(Card::Autotune, 0, 11.0f, p);
  check(p.pitchFx.autotune.key == 11, "autotune key position 11");
  applyKnob(Card::Octave, 0, 0.0f, p);
  check(p.pitchFx.octave.semitones == -12, "semitones position 0 is -12");
  applyKnob(Card::Octave, 0, 24.0f, p);
  check(p.pitchFx.octave.semitones == 12, "semitones position 24 is +12");
  check(knobSteps(Card::Octave, 0) == 25, "semitones has 25 positions");
  check(knobSteps(Card::Autotune, 0) == 12, "key has 12 positions");
  applyKnob(Card::Slapback, 0, 0.0f, p);
  check(std::fabs(p.slapback.tuning.timeMs - cv::Slapback::kTimeMinMs) < 1e-4f, "echo time min");
  applyKnob(Card::Slapback, 0, 1.0f, p);
  check(std::fabs(p.slapback.tuning.timeMs - cv::Slapback::kTimeMaxMs) < 1e-4f, "echo time max");
}

// A stepped value outside its positions, NaN included, lands on the nearest valid step.
void checkKnobClamps() {
  ChainParams p;
  applyKnob(Card::Autotune, 0, 99.0f, p);
  check(p.pitchFx.autotune.key == 11, "clamp: key above the last position");
  applyKnob(Card::Autotune, 0, -3.0f, p);
  check(p.pitchFx.autotune.key == 0, "clamp: key below the first position");
  applyKnob(Card::Autotune, 0, NAN, p);
  check(p.pitchFx.autotune.key == 0, "clamp: NaN key");
  applyKnob(Card::Octave, 0, 99.0f, p);
  check(p.pitchFx.octave.semitones == 12, "clamp: semitones above the last position");
  applyKnob(Card::Octave, 0, -1.0f, p);
  check(p.pitchFx.octave.semitones == -12, "clamp: semitones below the first position");

  const KnobDesc& key = *knobDesc(Card::Autotune, 0);
  check(std::strcmp(knobLabel(key, knobShown(key, 99.0f)), cv::kKeyName[11]) == 0, "clamp: key readout above the last position");
  check(std::strcmp(knobLabel(key, knobShown(key, -4.0f)), cv::kKeyName[0]) == 0, "clamp: key readout below the first position");
  check(knobShown(*knobDesc(Card::Octave, 0), 99.0f) == 12, "clamp: semitone readout above the last position");
}

void checkRegistryBounds() {
  check(knobDesc(Card::Count, 0) == nullptr && knobDesc(Card::Gate, kKnobsPerCard) == nullptr && knobDesc(Card::Gate, -1) == nullptr,
        "bounds: an out-of-range card or knob has no descriptor");
  check(knobSteps(Card::Count, 0) == 0 && knobSteps(Card::Autotune, kKnobsPerCard) == 0 && knobSteps(Card::Autotune, -1) == 0,
        "bounds: an invalid card or knob has no steps");

  // The constructor leaves padding unset, so the check compares against a byte copy of the same object.
  ChainParams after;
  unsigned char before[sizeof(ChainParams)];
  std::memcpy(before, &after, sizeof before);
  applyKnob(Card::Count, 0, 1.0f, after);
  applyKnob(Card::Gate, kKnobsPerCard, 1.0f, after);
  applyKnob(Card::Gate, -1, 1.0f, after);
  check(std::memcmp(before, &after, sizeof(ChainParams)) == 0, "bounds: an invalid card or knob changes nothing");
}

void checkChainDefaults() {
  const ChainParams p;
  const cv::GateTuning& t = p.inputGate.tuning;
  check(p.inputGate.on && std::fabs(p.inputGate.thresholdDb - (-55.0f)) < 1e-6f &&
            std::fabs(t.rangeDb - (-80.0f)) < 1e-6f && t.kneeDb == 0.0f &&
            std::fabs(t.holdMs - 30.0f) < 1e-6f && std::fabs(t.hysteresisDb - 3.0f) < 1e-6f &&
            std::fabs(t.attackMs - 0.5f) < 1e-6f && std::fabs(t.releaseMs - 200.0f) < 1e-6f &&
            std::fabs(t.detectorAttackMs - 0.1f) < 1e-6f && std::fabs(t.detectorReleaseMs - 5.0f) < 1e-6f &&
            t.detectorHpHz == 0.0f,
        "defaults: input gate on with the hard -80 dB range, 3 dB hysteresis and 30 ms hold");
  const cv::GateTuning& g = p.gate.tuning;
  check(g.kneeDb == 12.0f && g.rangeDb == -40.0f && g.hysteresisDb == 6.0f && g.holdMs == 150.0f &&
            g.detectorAttackMs == 0.0f && g.detectorReleaseMs == 20.0f && g.detectorHpHz == 120.0f &&
            t.rangeDb != g.rangeDb,
        "defaults: the Gate keeps its soft-knee tuning");

  // Both cards start at the middle of their pots.
  const ChainParams mid = [] {
    ChainParams m;
    applyKnob(Card::Gate, 0, 0.5f, m);
    applyKnob(Card::Gate, 1, 0.5f, m);
    return m;
  }();
  check(p.gate.thresholdDb == mid.gate.thresholdDb && std::fabs(g.releaseMs - mid.gate.tuning.releaseMs) < 1e-4f,
        "defaults: the Gate starts at its pots' middle, -40 dB and 70.7 ms");
  check(!p.gate.on && !p.pitchFx.autotune.on && !p.pitchFx.octave.on && !p.unison.on &&
            !p.slapback.on && !p.distortion.on && !p.reverb.on && !p.crowd.on,
        "defaults: every other stage off");
  check(std::fabs(p.pitchFx.octave.mix - 0.10f) < 1e-6f,
        "defaults: octave at 10 % mix");
  check(p.reverb.engine == cv::kReverbSpring && p.eq.on, "defaults: spring reverb engine and the EQ on");
}

void checkUnisonRate() {
  using unisonrate::lfoScale;
  check(std::fabs(lfoScale(0.0f) - 0.33f) < 1e-5f, "unison rate: 0 % scales the LFO by 0.33");
  check(std::fabs(lfoScale(50.0f) - 1.0f) < 1e-5f, "unison rate: 50 % scales by 1");
  check(std::fabs(lfoScale(100.0f) - 7.0f) < 1e-4f, "unison rate: 100 % scales by 7");
  check(lfoScale(-20.0f) == lfoScale(0.0f) && lfoScale(250.0f) == lfoScale(100.0f) && lfoScale(NAN) == lfoScale(0.0f),
        "unison rate: positions outside 0..100 clamp");
  check(lfoScale(25.0f) > lfoScale(0.0f) && lfoScale(25.0f) < lfoScale(50.0f) && lfoScale(75.0f) < lfoScale(100.0f),
        "unison rate: the scale rises between the anchors");

  cv::UnisonTuning t;
  unisonrate::apply(t, 50.0f);
  check(std::fabs(t.lfoHz[0] - 0.60f) < 1e-5f && std::fabs(t.lfoHz[1] - 0.90f) < 1e-5f, "unison rate: LFO speeds at 50 %");
  check(std::fabs(t.swingMinMs - 1.0f) < 1e-5f && std::fabs(t.swingMaxMs - 6.0f) < 1e-5f, "unison rate: swing at 50 %");
  unisonrate::apply(t, 100.0f);
  check(std::fabs(t.lfoHz[0] - 4.2f) < 1e-4f && std::fabs(t.swingMaxMs - 6.0f * 2.0f / 7.0f) < 1e-4f,
        "unison rate: fast rates keep the swing from growing");
  ChainParams p;
  applyKnob(Card::Unison, 0, 0.5f, p);
  check(std::fabs(p.unison.tuning.lfoHz[0] - 0.60f) < 1e-5f, "unison rate: the RATE knob drives the mapping");
}

void checkToggles() {
  ChainParams p;
  applyToggle(Card::Distortion, true, p);
  check(p.distortion.on, "distortion toggle on");
  applyToggle(Card::ReverbChasm, true, p);
  check(p.reverb.on, "reverb toggle on");
  applyToggle(Card::Autotune, true, p);
  applyToggle(Card::Octave, false, p);
  check(p.pitchFx.autotune.on && !p.pitchFx.octave.on, "pitch toggles are independent");
  applyToggle(Card::Crowd, true, p);
  check(p.crowd.on && p.pitchFx.autotune.on && !p.pitchFx.octave.on,
        "crowd toggle on sets its stage and leaves the pitch toggles alone");
  applyToggle(Card::Crowd, false, p);
  check(!p.crowd.on && p.pitchFx.autotune.on, "crowd toggle off clears its flag");
}

void checkPlacement() {
  ChainParams p;
  applyPlacement(Card::ReverbChasm, p);
  check(p.reverb.engine == cv::kReverbChasm, "chasm card selects chasm engine");
  applyPlacement(Card::ReverbSpring, p);
  check(p.reverb.engine == cv::kReverbSpring, "spring card selects spring engine");
  applyPlacement(Card::Unison, p);
  check(p.reverb.engine == cv::kReverbSpring, "non-reverb card leaves the engine alone");
}

std::string cellsOf(const KnobDesc& k, int shown) {
  char buf[kMaxKnobCells + 1];
  formatCells(k, shown, buf, sizeof(buf));
  return buf;
}

// What the OLED reads for a reading: the name of a labelled knob, else the number's cells with the
// blank ones dropped.
std::string shownText(const KnobDesc& k, float reading) {
  const int shown = knobShown(k, reading);
  if (k.labels) return knobLabel(k, shown);
  std::string text = cellsOf(k, shown);
  text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
  return text;
}

void checkShownNumbers() {
  const KnobDesc& semitones = *knobDesc(Card::Octave, 0);
  check(shownText(semitones, 17.0f) == "+5" && shownText(semitones, 12.0f) == "0" && shownText(semitones, 0.0f) == "-12",
        "shown: semitones read +5, 0 and -12");
  check(knobShown(semitones, 24.0f) == 12 && knobShown(semitones, 99.0f) == 12 && knobShown(semitones, -3.0f) == -12 &&
            knobShown(semitones, NAN) == -12,
        "shown: a stepped reading clamps to the positions");

  const KnobDesc& threshold = *knobDesc(Card::Gate, 0);
  check(shownText(threshold, 0.0f) == "-70" && shownText(threshold, 1.0f) == "-10" && knobShown(threshold, 0.5f) == -40,
        "shown: a threshold reads whole dB with no plus");
  const KnobDesc& mix = *knobDesc(Card::Octave, 1);
  check(shownText(mix, 0.0f) == "0" && shownText(mix, 0.65f) == "65" && shownText(mix, 1.0f) == "100",
        "shown: a percent reads whole numbers and no plus");
  check(knobShown(mix, 5.0f) == 100 && knobShown(mix, -1.0f) == 0 && knobShown(mix, NAN) == 0,
        "shown: a continuous reading clamps to 0..1");
  const KnobDesc& time = *knobDesc(Card::Slapback, 0);
  check(knobShown(time, 0.0f) == static_cast<int>(cv::Slapback::kTimeMinMs) &&
            knobShown(time, 1.0f) == static_cast<int>(cv::Slapback::kTimeMaxMs),
        "shown: echo time spans its range");
}

void checkShownLabels() {
  const KnobDesc& key = *knobDesc(Card::Autotune, 0);
  check(key.labels != nullptr && std::strcmp(key.labels[11], cv::kKeyName[11]) == 0 && knobShown(key, 11.0f) == 11,
        "shown: the key reads its step index");
  check(shownText(key, 11.0f) == "F / Dm" && shownText(key, 0.0f) == "C / Am", "shown: the key reads its name");
  check(shownText(key, 99.0f) == cv::kKeyName[11] && shownText(key, -2.0f) == cv::kKeyName[0],
        "shown: a key reading clamps to the first and last name");
  check(std::strcmp(knobLabel(key, 99), cv::kKeyName[11]) == 0 && std::strcmp(knobLabel(key, -5), cv::kKeyName[0]) == 0,
        "label: an index clamps to the positions");
  check(std::strcmp(knobLabel(*knobDesc(Card::Octave, 0), 3), "") == 0, "label: a knob without labels has none");
}

// The Mob card's knob A is DIVERGE, a percent. Knob B is VOICES, ten positions named 0 to 8 and MOB.
void checkCrowdVoices() {
  const KnobDesc& diverge = *knobDesc(Card::Crowd, 0);
  check(std::strcmp(diverge.name, "DIVERGE") == 0 && diverge.kind == KnobKind::Continuous,
        "crowd: knob A is the continuous DIVERGE");
  const KnobDesc& voices = *knobDesc(Card::Crowd, 1);
  check(std::strcmp(voices.name, "VOICES") == 0 && voices.kind == KnobKind::Stepped && voices.positions == 10 &&
            knobSteps(Card::Crowd, 1) == 10,
        "crowd: knob B is VOICES with 10 positions");
  const char* const names[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "MOB"};
  bool named = voices.labels != nullptr;
  for (int i = 0; named && i < 10; ++i) {
    named = std::strcmp(voices.labels[i], names[i]) == 0 && knobShown(voices, static_cast<float>(i)) == i &&
            shownText(voices, static_cast<float>(i)) == names[i];
  }
  check(named, "crowd: VOICES reads 0 to 8 and MOB");
  check(shownText(voices, 99.0f) == "MOB" && shownText(voices, -2.0f) == "0",
        "crowd: a VOICES reading clamps to 0 and MOB");

  // Each step index reaches the engine as itself, and the top of the knob is MOB.
  bool reaches = true;
  for (int i = 0; i < 10; ++i) {
    ChainParams p;
    applyKnob(Card::Crowd, 1, static_cast<float>(i), p);
    reaches = reaches && p.crowd.voicesStep == i;
  }
  check(reaches, "crowd: each VOICES step reaches the engine as its index");
  ChainParams top;
  applyKnob(Card::Crowd, 1, 99.0f, top);
  check(top.crowd.voicesStep == cv::kVoicesMob, "crowd: a VOICES reading past the top lands on MOB");
}

// Formant and the Formant follower, alternates for Octave in slot 3, and Lo-fi and Chatter, alternates for
// Gate in slot 7. They sit after the Crowd card in the table, so every earlier id keeps its meaning in a
// saved layout. Each row is the estimate in costs.h until the stress run replaces it.
void checkAppendedCards() {
  check(static_cast<int>(Card::Gate) == 7 && static_cast<int>(Card::Crowd) == 10 && static_cast<int>(Card::Formant) == 11 &&
            static_cast<int>(Card::Lofi) == 12 && static_cast<int>(Card::Bloom) == 13 &&
            static_cast<int>(Card::FormantFollower) == 14 && static_cast<int>(Card::Growl) == 15 &&
            kCardCount == 16,
        "appended cards: the table appends them after the Crowd card");
  check(static_cast<int>(Engine::Crowd) == 9 && static_cast<int>(Engine::Lofi) == 10 && static_cast<int>(Engine::Bloom) == 11 &&
            static_cast<int>(Engine::Growl) == 12 &&
            kEngineCount == 13,
        "appended cards: the engine list appends Lofi, Bloom (the Chatter card's engine) and Growl after the Crowd engine");
  const auto sameRow = [](Card c, const costs::Cost& row) {
    return cardDesc(c).cost.avg == row.avg && cardDesc(c).cost.worst == row.worst && cardDesc(c).cost.measured == row.measured;
  };
  check(sameRow(Card::Formant, costs::kFormant) && sameRow(Card::Lofi, costs::kLofi) && sameRow(Card::Bloom, costs::kBloom) &&
            sameRow(Card::FormantFollower, costs::kFormantFollower),
        "appended cards: each registry row is its costs.h row");
}

// Apraxia: id formant_follow, which keeps under the id clip width, the Formant engine and the pitch group, SENSITIVITY
// on the upper pot, RANGE on the lower from -12 st at the pot's bottom through 0 at noon to +12 st at its
// top, a straight line on each side of noon, no knob on the Formant voice's MIX, and Apraxia on the OLED's
// readout title, as Mob's row does. Placing it hands the voice's SHIFT to the card's sweep.
void checkApraxiaCard() {
  const CardDesc& d = cardDesc(Card::FormantFollower);
  check(std::strcmp(d.id, "formant_follow") == 0 && std::strcmp(d.name, "Apraxia") == 0 && d.engine == Engine::Formant &&
            d.group == Group::Pitch && d.knobCount == 2,
        "apraxia: id formant_follow, name Apraxia, the Formant engine and the pitch group");
  check(d.oledName == nullptr && std::strcmp(oledName(Card::FormantFollower), "Apraxia") == 0,
        "apraxia: the readout title is the card's name, Apraxia");
  const KnobDesc& sens = *knobDesc(Card::FormantFollower, 0);
  check(std::strcmp(sens.name, "SENSITIVITY") == 0 && sens.kind == KnobKind::Continuous && sens.min == 0.0f &&
            sens.max == 100.0f && std::strcmp(sens.unit, "%") == 0,
        "apraxia: knob A is SENSITIVITY, a percent");
  const KnobDesc& range = *knobDesc(Card::FormantFollower, 1);
  check(std::strcmp(range.name, "RANGE") == 0 && range.kind == KnobKind::Continuous && range.min == -12.0f &&
            range.max == 12.0f && range.curve == Curve::ZeroAtNoon && std::strcmp(range.unit, "st") == 0,
        "apraxia: knob B is RANGE, continuous from -12 st at the bottom through 0 at noon to +12 st at the top");

  ChainParams p;
  const float mixBefore = p.pitchFx.formant.mix;
  applyKnob(Card::FormantFollower, 0, 0.3f, p);
  check(std::fabs(p.follower.sensitivity - 0.3f) < 1e-6f, "apraxia: SENSITIVITY hands the card its position");
  const auto rangeAt = [&p](float position) {
    applyKnob(Card::FormantFollower, 1, position, p);
    return p.follower.rangeSt;
  };
  check(rangeAt(0.0f) == -12.0f && rangeAt(0.25f) == -6.0f && rangeAt(0.5f) == 0.0f && rangeAt(0.75f) == 6.0f &&
            rangeAt(1.0f) == 12.0f,
        "apraxia: RANGE is -12 st at the bottom, -6 st at 25 %, 0 at noon, +6 st at 75 % and +12 st at the top");
  const float defaultPosition = 0.5f * (1.0f - cv::kFollowerDefaultRangeSt / cv::kFollowerMinRangeSt);
  check(cv::kFollowerDefaultRangeSt == -5.0f && std::fabs(rangeAt(defaultPosition) + 5.0f) < 1e-5f &&
            std::fabs(defaultPosition - 7.0f / 24.0f) < 1e-6f && shownText(range, defaultPosition) == "-5",
        "apraxia: the default pot position, 7/24 of the travel, reads -5 st");
  bool capped = true;
  bool rising = true;
  float before = -13.0f;
  for (int s = 0; s <= 100; ++s) {
    const float st = rangeAt(static_cast<float>(s) / 100.0f);
    capped = capped && st >= -12.0f && st <= 12.0f;
    rising = rising && st > before;
    before = st;
  }
  check(capped && rising, "apraxia: RANGE rises with the pot and never leaves -12 to +12 st");
  check(shownText(range, 0.0f) == "-12" && shownText(range, 0.25f) == "-6" && shownText(range, 0.5f) == "0" &&
            shownText(range, 0.75f) == "+6" && shownText(range, 1.0f) == "+12" && knobCells(range) == 3,
        "apraxia: RANGE reads its semitones with the sign in three cells");
  check(p.pitchFx.formant.mix == mixBefore, "apraxia: no knob sets the Formant voice's mix");

  ChainParams t;
  applyToggle(Card::FormantFollower, true, t);
  check(t.pitchFx.formant.on && !t.pitchFx.autotune.on && !t.pitchFx.octave.on && !t.follower.placed,
        "apraxia: its toggle sets formant.on and nothing else");
  check(!ChainParams().follower.placed, "apraxia: the sweep drives nothing before the card is placed");
  applyPlacement(Card::Formant, t);
  check(!t.follower.placed, "apraxia: placing the Formant card leaves SHIFT on its own knob");
  applyPlacement(Card::FormantFollower, t);
  check(t.follower.placed, "apraxia: placing the card hands SHIFT to the sweep");
}

void checkFormantCard() {
  const CardDesc& formant = cardDesc(Card::Formant);
  check(std::strcmp(formant.id, "formant") == 0 && std::strcmp(formant.name, "Formant") == 0 &&
            formant.engine == Engine::Formant && formant.group == Group::Pitch && formant.knobCount == 2,
        "formant: id formant, name Formant, the Formant engine and the pitch group");
  const KnobDesc& shift = *knobDesc(Card::Formant, 0);
  check(std::strcmp(shift.name, "SHIFT") == 0 && shift.kind == KnobKind::Stepped && shift.positions == 15 &&
            knobSteps(Card::Formant, 0) == 15 && shift.min == -7.0f && shift.max == 7.0f &&
            std::strcmp(shift.unit, "st") == 0,
        "formant: knob A is SHIFT, 15 positions from -7 to +7 st");
  const KnobDesc& fmix = *knobDesc(Card::Formant, 1);
  check(std::strcmp(fmix.name, "MIX") == 0 && fmix.kind == KnobKind::Continuous && std::strcmp(fmix.unit, "%") == 0 &&
            fmix.min == 0.0f && fmix.max == 100.0f,
        "formant: knob B is MIX, a continuous percent");

  // SHIFT lands on whole semitones, -7 at the first position, 0 at the middle one and +7 at the last.
  ChainParams p;
  const auto shiftAt = [&p](float position) {
    applyKnob(Card::Formant, 0, position, p);
    return p.pitchFx.formant.shiftSt;
  };
  check(shiftAt(0.0f) == -7 && shiftAt(7.0f) == 0 && shiftAt(14.0f) == 7, "formant: SHIFT position 0 is -7 st, 7 is 0 and 14 is +7");
  check(shiftAt(99.0f) == 7, "formant: SHIFT above the last position clamps to +7");
  check(shiftAt(NAN) == -7, "formant: a NaN SHIFT lands on the first position");
  bool wholeSteps = true;
  for (int i = 0; i < 15; ++i) wholeSteps = wholeSteps && shiftAt(static_cast<float>(i)) == i - 7;
  check(wholeSteps, "formant: every SHIFT position i lands on i - 7 st");
  check(shownText(shift, 4.0f) == "-3" && shownText(shift, 7.0f) == "0" && shownText(shift, 12.0f) == "+5" &&
            knobCells(shift) == 2,
        "formant: SHIFT reads -3, 0 and +5 in two cells");
  applyKnob(Card::Formant, 1, 0.4f, p);
  check(std::fabs(p.pitchFx.formant.mix - 0.4f) < 1e-6f, "formant: MIX drives the voice's mix");
  check(shownText(fmix, 1.0f) == "100", "formant: MIX 100 % reads 100");
}

void checkLofiCard() {
  const CardDesc& lofi = cardDesc(Card::Lofi);
  check(std::strcmp(lofi.id, "lofi") == 0 && std::strcmp(lofi.name, "Lo-fi") == 0 && lofi.engine == Engine::Lofi &&
            lofi.group == Group::None && lofi.knobCount == 2,
        "lofi: id lofi, name Lo-fi, its own engine, no group");
  check(std::strcmp(knobDesc(Card::Lofi, 0)->name, "TONE") == 0 && std::strcmp(knobDesc(Card::Lofi, 1)->name, "AMOUNT") == 0 &&
            knobSteps(Card::Lofi, 0) == 0 && knobSteps(Card::Lofi, 1) == 0 &&
            std::strcmp(knobDesc(Card::Lofi, 0)->unit, "%") == 0 && std::strcmp(knobDesc(Card::Lofi, 1)->unit, "%") == 0,
        "lofi: knob A is TONE and knob B is AMOUNT, both continuous percents");

  ChainParams p;
  applyKnob(Card::Lofi, 0, 0.3f, p);
  applyKnob(Card::Lofi, 1, 0.8f, p);
  check(std::fabs(p.lofi.tone - 0.3f) < 1e-6f && std::fabs(p.lofi.amount - 0.8f) < 1e-6f,
        "lofi: TONE and AMOUNT drive their own fields, both as the pot position");
}

void checkBloomCard() {
  const CardDesc& bloom = cardDesc(Card::Bloom);
  check(std::strcmp(bloom.id, "bloom") == 0 && std::strcmp(bloom.name, "Chatter") == 0 && bloom.engine == Engine::Bloom &&
            bloom.group == Group::None && bloom.knobCount == 2,
        "chatter: id bloom, name Chatter, its own engine, no group");
  const KnobDesc& length = *knobDesc(Card::Bloom, 0);
  check(std::strcmp(length.name, "LENGTH") == 0 && length.kind == KnobKind::Continuous && length.curve == Curve::Log &&
            length.min == 20.0f && length.max == 3000.0f && std::strcmp(length.unit, "ms") == 0,
        "chatter: knob A is LENGTH, 20 to 3000 ms on a log curve");
  check(std::strcmp(knobDesc(Card::Bloom, 1)->name, "AMOUNT") == 0 && std::strcmp(knobDesc(Card::Bloom, 1)->unit, "%") == 0,
        "chatter: knob B is AMOUNT, a percent");

  ChainParams p;
  applyKnob(Card::Bloom, 0, 0.0f, p);
  const float lengthLow = p.bloom.lengthMs;
  applyKnob(Card::Bloom, 0, 1.0f, p);
  const float lengthHigh = p.bloom.lengthMs;
  applyKnob(Card::Bloom, 0, 0.5f, p);
  check(std::fabs(lengthLow - 20.0f) < 1e-3f && std::fabs(lengthHigh - 3000.0f) < 1e-1f &&
            std::fabs(p.bloom.lengthMs - std::sqrt(20.0f * 3000.0f)) < 1e-2f,
        "chatter: LENGTH runs 20 to 3000 ms with its middle at the geometric mean");
  applyKnob(Card::Bloom, 1, 0.65f, p);
  check(std::fabs(p.bloom.amount - 0.65f) < 1e-6f && std::fabs(p.bloom.lengthMs - std::sqrt(20.0f * 3000.0f)) < 1e-2f,
        "chatter: AMOUNT drives the amount field and leaves LENGTH alone");
  check(shownText(length, 0.0f) == "20" && shownText(length, 1.0f) == "3000" && knobCells(length) == 4,
        "chatter: LENGTH reads 20 to 3000 in four cells");
}

// Growl, an alternate for Distortion in slot 6, sits in the pitch group with an engine of its own, and its
// toggle keeps the pitch stage's ring and tracker running as the Crowd card's does.
void checkGrowlCard() {
  const CardDesc& growl = cardDesc(Card::Growl);
  check(std::strcmp(growl.id, "growl") == 0 && std::strcmp(growl.name, "Deepthroat") == 0 && growl.engine == Engine::Growl &&
            growl.group == Group::Pitch && growl.knobCount == 2,
        "growl: id growl, name Deepthroat, its own engine, the pitch group");
  check(std::strcmp(knobDesc(Card::Growl, 0)->name, "GROWL") == 0 && std::strcmp(knobDesc(Card::Growl, 1)->name, "MIX") == 0 &&
            knobSteps(Card::Growl, 0) == 0 && knobSteps(Card::Growl, 1) == 0 &&
            std::strcmp(knobDesc(Card::Growl, 0)->unit, "%") == 0 && std::strcmp(knobDesc(Card::Growl, 1)->unit, "%") == 0,
        "growl: knob A is GROWL and knob B is MIX, both continuous percents");
  check(growl.cost.avg == costs::kGrowl.avg && growl.cost.worst == costs::kGrowl.worst &&
            growl.cost.measured == costs::kGrowl.measured,
        "growl: the registry row is its costs.h row");

  ChainParams p;
  applyKnob(Card::Growl, 0, 0.3f, p);
  applyKnob(Card::Growl, 1, 0.8f, p);
  check(std::fabs(p.growl.growl - 0.3f * 0.80f) < 1e-6f && std::fabs(p.growl.drive - 0.3f * 0.65f) < 1e-6f &&
            std::fabs(p.growl.mix - 0.8f) < 1e-6f,
        "growl: the GROWL pot sets the rasp at 0.80 and the fold at 0.65 of its position, and MIX its own field");
  for (const float pot : {0.0f, 0.5f, 1.0f}) {
    applyKnob(Card::Growl, 0, pot, p);
    check(std::fabs(p.growl.growl - 0.80f * pot) < 1e-6f && std::fabs(p.growl.drive - 0.65f * pot) < 1e-6f,
          "growl: GROWL 0 / 50 / 100 % gives rasp 0 / 0.40 / 0.80 and drive 0 / 0.325 / 0.65");
  }
  applyToggle(Card::Growl, true, p);
  check(p.growl.on && !p.crowd.on && !p.distortion.on, "growl: its toggle turns the card on and nothing else");
  applyToggle(Card::Growl, false, p);
  check(!p.growl.on, "growl: and off again");
}

void checkAppendedToggles() {
  // The toggles reach their own stage. Formant's toggle sits inside the pitch parameters and touches no
  // other pitch toggle.
  ChainParams t;
  applyToggle(Card::Formant, true, t);
  check(t.pitchFx.formant.on && !t.pitchFx.autotune.on && !t.pitchFx.octave.on && !t.crowd.on && !t.lofi.on &&
            !t.bloom.on,
        "formant: its toggle sets formant.on and nothing else");
  applyToggle(Card::Lofi, true, t);
  applyToggle(Card::Bloom, true, t);
  check(t.lofi.on && t.bloom.on && t.pitchFx.formant.on, "lofi and chatter: each toggle sets its own on flag");
  applyToggle(Card::Formant, false, t);
  check(!t.pitchFx.formant.on && t.lofi.on && t.bloom.on, "formant: its toggle off clears only its own flag");

  // The defaults before any card is applied: all three stages off.
  const ChainParams fresh;
  check(!fresh.pitchFx.formant.on && !fresh.lofi.on && !fresh.bloom.on && !fresh.bloom.keyFromGate,
        "defaults: Formant, Lo-fi and Chatter off, and Chatter listens to its own input");

  // Placing the Input gate makes it the Chatter card's word key. Placing the card alone does not.
  ChainParams keyed;
  applyPlacement(Card::Bloom, keyed);
  check(!keyed.bloom.keyFromGate, "chatter: placing the card alone leaves it on its own detector");
  applyPlacement(Card::InputGate, keyed);
  check(keyed.bloom.keyFromGate, "chatter: placing the Input gate makes its detector the word key");
}

// No card declares decimals yet, so a made-up knob stands in.
void checkShownDecimals() {
  const KnobDesc rate = {"Rate", KnobKind::Continuous, 0, 0.1f, 5.0f, "hz", Curve::Linear, 1};
  check(knobShown(rate, 0.0f) == 1 && knobShown(rate, 1.0f) == 50 && shownText(rate, 0.0f) == "0.1" &&
            shownText(rate, 1.0f) == "5.0",
        "shown: one decimal counts in tenths and prints a point");
  const KnobDesc bipolar = {"Trim", KnobKind::Continuous, 0, -2.0f, 2.0f, "db", Curve::Linear, 2};
  check(shownText(bipolar, 0.0f) == "-2.00" && shownText(bipolar, 0.5f) == "0.00" && shownText(bipolar, 0.6f) == "+0.40" &&
            shownText(bipolar, 0.1f) == "-1.60",
        "shown: two decimals keep their zeros, a minus and a plus");
}

// The cells of a knob's number are fixed by its range. Each value fills all of them.
void checkCells() {
  struct Case {
    Card card;
    int knob;
    int cells;
  };
  const Case cases[] = {
      {Card::Octave, 0, 3},      // -12..+12 is a sign cell and 2 digits
      {Card::Octave, 1, 3},      // 0..100 %
      {Card::Gate, 0, 3},        // -70..-10 dB is a sign cell and 2 digits
      {Card::Gate, 1, 4},        // 5..1000 ms
      {Card::InputGate, 0, 3},   // -80..-30 dB is a sign cell and 2 digits
      {Card::InputGate, 1, 4},   // 20..2000 ms
      {Card::Slapback, 0, 3},    // 60..250 ms
      {Card::Autotune, 1, 3},    // 5..500 ms response
      {Card::Distortion, 1, 3},  // 0..100 %
      {Card::Autotune, 0, 0},    // the key has names, not a number
  };
  for (const Case& c : cases) {
    const std::string what = std::string("cells: ") + cardDesc(c.card).name + " " + cardDesc(c.card).knob[c.knob].name;
    check(knobCells(*knobDesc(c.card, c.knob)) == c.cells, (what + " takes " + std::to_string(c.cells) + " cells").c_str());
  }
}

void checkFormatCells() {
  const KnobDesc& semitones = *knobDesc(Card::Octave, 0);
  check(cellsOf(semitones, 5) == "+ 5" && cellsOf(semitones, 0) == "  0" && cellsOf(semitones, -5) == "- 5" &&
            cellsOf(semitones, 12) == "+12" && cellsOf(semitones, -12) == "-12",
        "cells: semitones keep the sign in its own cell and a blank tens cell for one digit");
  const KnobDesc& threshold = *knobDesc(Card::Gate, 0);
  check(cellsOf(threshold, -24) == "-24" && cellsOf(threshold, -70) == "-70" && cellsOf(threshold, -10) == "-10",
        "cells: a threshold is the minus and its two digits");
  const KnobDesc& mix = *knobDesc(Card::Octave, 1);
  check(cellsOf(mix, 0) == "  0" && cellsOf(mix, 65) == " 65" && cellsOf(mix, 99) == " 99" && cellsOf(mix, 100) == "100",
        "cells: a percent is right-aligned with blank cells on the left, and no sign cell");
  const KnobDesc& decay = *knobDesc(Card::Gate, 1);
  check(cellsOf(decay, 5) == "   5" && cellsOf(decay, 150) == " 150" && cellsOf(decay, 1000) == "1000", "cells: decay has 4 digit cells");

  // A number wider than its cells keeps the rightmost characters, and a negative number in a range with no sign loses its minus.
  check(cellsOf(mix, 1234) == "234" && cellsOf(mix, -7) == "  7", "cells: a number past the field keeps its low digits");
  check(cellsOf(*knobDesc(Card::Autotune, 0), 3).empty(), "cells: a knob with names has no cells");
  char tiny[3] = "xx";
  formatCells(semitones, 5, tiny, sizeof(tiny));
  check(tiny[0] == '\0', "cells: a buffer too small for the cells gets an empty string");
  formatCells(semitones, 5, tiny, 0);

  // The cells say the number as the spec writes it: a minus below zero, a plus above it on a signed
  // range, and no sign for zero.
  bool agree = true;
  for (int v = -12; v <= 12; ++v) {
    const std::string written = (v < 0 ? "-" : (v > 0 ? "+" : "")) + std::to_string(std::abs(v));
    std::string cells = cellsOf(semitones, v);
    cells.erase(std::remove(cells.begin(), cells.end(), ' '), cells.end());
    agree = agree && cells == written;
  }
  check(agree, "cells: dropping the blanks of a number's cells leaves the number as written");
}

// The cells follow whichever end of the range is wider. No card's lower end is wider than its upper one.
void checkCellsWiderLowEnd() {
  const KnobDesc deep = {"Deep", KnobKind::Continuous, 0, -100.0f, 5.0f, "db"};
  check(knobCells(deep) == 4 && cellsOf(deep, -100) == "-100" && cellsOf(deep, 5) == "+  5" && cellsOf(deep, -7) == "-  7",
        "cells: a range whose lower end has more digits sizes the field for it");
  const KnobDesc high = {"High", KnobKind::Continuous, 0, -5.0f, 100.0f, "db"};
  check(knobCells(high) == 4 && cellsOf(high, 100) == "+100" && cellsOf(high, -5) == "-  5",
        "cells: and so does a range whose upper end has more digits");
}

// A knob with decimals counts a point cell and one cell for each decimal. No card has one yet.
void checkCellsDecimals() {
  const KnobDesc rate = {"Rate", KnobKind::Continuous, 0, 0.1f, 5.0f, "hz", Curve::Linear, 1};
  check(knobCells(rate) == 3 && cellsOf(rate, 15) == "1.5" && cellsOf(rate, 1) == "0.1" && cellsOf(rate, 50) == "5.0",
        "cells: one decimal makes 3 cells, the point included");
  const KnobDesc trim = {"Trim", KnobKind::Continuous, 0, -2.0f, 2.0f, "db", Curve::Linear, 2};
  check(knobCells(trim) == 5 && cellsOf(trim, -160) == "-1.60" && cellsOf(trim, 40) == "+0.40" && cellsOf(trim, 0) == " 0.00",
        "cells: a signed range with two decimals is a sign cell and 4 more");
  const KnobDesc wide = {"Wide", KnobKind::Continuous, 0, 0.0f, 1000.0f, "hz", Curve::Linear, 2};
  check(knobCells(wide) == 7 && cellsOf(wide, 1) == "   0.01", "cells: a leading zero stays before the point");
}

// The field of every knob must fit the panel with 1 px each side: its cells and unit in 16X24, or for a
// knob with names, the widest name.
void checkReadoutsFit() {
  const FontDesc& big = font16x24();
  bool fits = true;
  for (int c = 1; c < kCardCount; ++c) {
    for (int k = 0; k < cardDesc(card(c)).knobCount; ++k) {
      const KnobDesc& kd = *knobDesc(card(c), k);
      int width = 0;
      if (kd.labels) {
        for (int i = 0; i < kd.positions; ++i) width = std::max(width, Canvas::textWidth(big, kd.labels[i]));
      } else {
        const std::string cells(static_cast<size_t>(knobCells(kd)), '8');
        const std::string unit = std::string(" ") + kd.unit;
        width = Canvas::digitsWidth(big, cells.c_str()) + big.spacing + Canvas::textWidth(big, unit.c_str());
      }
      fits = fits && width <= kCanvasWidth - 2;
    }
  }
  check(fits, "readout: every knob's field fits the panel in 16X24");
}

}  // namespace

void checkRegistry() {
  checkIds();
  checkCardTable();
  checkGroups();
  checkAmountKnobIsB();
  checkKnobsMove();
  checkKnobTargets();
  checkKnobClamps();
  checkRegistryBounds();
  checkDescriptors();
  checkChainDefaults();
  checkUnisonRate();
  checkToggles();
  checkPlacement();
  checkShownNumbers();
  checkShownLabels();
  checkCrowdVoices();
  checkAppendedCards();
  checkFormantCard();
  checkApraxiaCard();
  checkLofiCard();
  checkBloomCard();
  checkGrowlCard();
  checkAppendedToggles();
  checkShownDecimals();
  checkCells();
  checkFormatCells();
  checkCellsWiderLowEnd();
  checkCellsDecimals();
  checkReadoutsFit();
}
