// Checks the card registry on the host. Covers ids, descriptors, knob mapping, clamps, chain
// defaults and readouts.

#include "support.h"

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
  check(engineOf(Card::Autotune) == Engine::Pitch && engineOf(Card::Octave) == Engine::Pitch,
        "table: both pitch cards share the pitch engine");
  check(engineOf(Card::ReverbSpring) == Engine::Reverb && engineOf(Card::ReverbChasm) == Engine::Reverb,
        "table: both reverb cards share the reverb engine");
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
  check(reverbs == 2, "two reverb cards");
  check(pitch == 2, "autotune and octave are the pitch group");
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
    case Card::Unison: return knob == 0 ? p.unison.depth : p.unison.tuning.lfoHz[0];
    case Card::Slapback: return knob == 0 ? p.slapback.intensity : p.slapback.tuning.timeMs;
    case Card::Distortion: return knob == 0 ? p.distortion.drive : p.distortion.tone;
    case Card::ReverbSpring:
    case Card::ReverbChasm: return knob == 0 ? p.reverb.intensity : p.reverb.time;
    case Card::Empty:
    case Card::Count: break;
  }
  return 0.0f;
}

// Descriptor, formatKnob and applyKnob must tell the same story for every continuous knob.
void checkDescriptors() {
  for (int i = 1; i < kCardCount; ++i) {
    const Card c = card(i);
    for (int k = 0; k < cardDesc(c).knobCount; ++k) {
      const KnobDesc& d = cardDesc(c).knob[k];
      if (d.kind != KnobKind::Continuous) continue;
      char what[96];
      check(d.min < d.max, "descriptor min below max");
      const bool reversed = d.curve == Curve::LogReversed;
      char lo[24];
      char hi[24];
      formatKnob(c, k, 0.0f, lo, sizeof(lo));
      formatKnob(c, k, 1.0f, hi, sizeof(hi));
      std::snprintf(what, sizeof(what), "%s knob %d format at 0 and 1", cardDesc(c).id, k);
      check(std::atoi(lo) == std::lround(reversed ? d.max : d.min) && std::atoi(hi) == std::lround(reversed ? d.min : d.max), what);

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
  check(knobSteps(Card::Autotune, 0) == static_cast<int>(sizeof(cv::kKeyName) / sizeof(cv::kKeyName[0])),
        "key positions equal cv::kKeyName");
}

void checkKnobTargets() {
  ChainParams p;
  applyKnob(Card::InputGate, 0, 1.0f, p);
  check(std::fabs(p.inputGate.thresholdDb - (-10.0f)) < 1e-4f, "input gate threshold max -10 dB");
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
  applyKnob(Card::Slapback, 1, 0.0f, p);
  check(std::fabs(p.slapback.tuning.timeMs - 60.0f) < 1e-4f, "slapback time min 60 ms");
  applyKnob(Card::Slapback, 1, 1.0f, p);
  check(std::fabs(p.slapback.tuning.timeMs - 250.0f) < 1e-4f, "slapback time max 250 ms");
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

  char buf[32];
  formatKnob(Card::Autotune, 0, 99.0f, buf, sizeof(buf));
  check(std::strcmp(buf, cv::kKeyName[11]) == 0, "clamp: key readout above the last position");
  formatKnob(Card::Autotune, 0, -4.0f, buf, sizeof(buf));
  check(std::strcmp(buf, cv::kKeyName[0]) == 0, "clamp: key readout below the first position");
  formatKnob(Card::Octave, 0, 99.0f, buf, sizeof(buf));
  check(std::strcmp(buf, "+12 st") == 0, "clamp: semitone readout above the last position");
}

void checkRegistryBounds() {
  char buf[32] = "unchanged";
  formatKnob(Card::Count, 0, 0.5f, buf, sizeof(buf));
  check(buf[0] == '\0', "bounds: an out-of-range card formats as empty");
  std::strcpy(buf, "unchanged");
  formatKnob(Card::Gate, kKnobsPerCard, 0.5f, buf, sizeof(buf));
  check(buf[0] == '\0', "bounds: a knob past the card formats as empty");
  formatKnob(Card::Gate, -1, 0.5f, buf, 0);
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
  check(p.inputGate.on && std::fabs(p.inputGate.thresholdDb - (-12.0f)) < 1e-6f &&
            std::fabs(p.inputGate.tuning.rangeDb - (-12.0f)) < 1e-6f &&
            std::fabs(p.inputGate.tuning.holdMs - 100.0f) < 1e-6f &&
            std::fabs(p.inputGate.tuning.releaseMs - 10.0f) < 1e-6f,
        "defaults: input gate on with the live-vocal settings");
  check(!p.gate.on && !p.pitchFx.autotune.on && !p.pitchFx.harmony.on && !p.pitchFx.octave.on && !p.unison.on &&
            !p.slapback.on && !p.distortion.on && !p.reverb.on,
        "defaults: every other stage off");
  check(p.pitchFx.octave.engine == 2 && std::fabs(p.pitchFx.octave.mix - 0.10f) < 1e-6f,
        "defaults: octave engine C at 10 % mix");
  check(p.reverb.engine == cv::kReverbSpringB && p.eq.on, "defaults: spring B reverb engine and the EQ on");
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
  check(t.detuneCents[0] == 0.0f && t.detuneCents[1] == 0.0f && !std::signbit(t.detuneCents[1]), "unison rate: no detune");
  unisonrate::apply(t, 100.0f);
  check(std::fabs(t.lfoHz[0] - 4.2f) < 1e-4f && std::fabs(t.swingMaxMs - 6.0f * 2.0f / 7.0f) < 1e-4f,
        "unison rate: fast rates keep the swing from growing");
  ChainParams p;
  applyKnob(Card::Unison, 1, 0.5f, p);
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
  check(!p.pitchFx.harmony.on, "harmony stays off");
}

void checkPlacement() {
  ChainParams p;
  applyPlacement(Card::ReverbChasm, p);
  check(p.reverb.engine == cv::kReverbChasm, "chasm card selects chasm engine");
  applyPlacement(Card::ReverbSpring, p);
  check(p.reverb.engine == cv::kReverbSpringB, "spring card selects spring B engine");
  applyPlacement(Card::Unison, p);
  check(p.reverb.engine == cv::kReverbSpringB, "non-reverb card leaves the engine alone");
}

void checkFormat() {
  char buf[32];
  formatKnob(Card::Autotune, 0, 11.0f, buf, sizeof(buf));
  check(std::strcmp(buf, "F / Dm") == 0, "key 11 formats as F / Dm");
  formatKnob(Card::Octave, 0, 12.0f, buf, sizeof(buf));
  check(std::strcmp(buf, "0 st") == 0, "semitones centre formats as 0 st, with no sign");
  formatKnob(Card::Gate, 0, 1.0f, buf, sizeof(buf));
  check(std::strcmp(buf, "-10 dB") == 0, "gate threshold formats in dB");
  formatKnob(Card::Slapback, 1, 0.0f, buf, sizeof(buf));
  check(std::strcmp(buf, "60 ms") == 0, "slapback time formats in ms");
  formatKnob(Card::Distortion, 0, 0.5f, buf, sizeof(buf));
  check(std::strcmp(buf, "50 %") == 0, "drive formats in percent");
}

// What the OLED reads for a reading, as numbers and names from the descriptor.
std::string shownText(const KnobDesc& k, float reading) {
  char buf[16];
  formatShown(k, knobShown(k, reading), buf, sizeof(buf));
  return buf;
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
  const KnobDesc& time = *knobDesc(Card::Slapback, 1);
  check(knobShown(time, 0.0f) == 60 && knobShown(time, 1.0f) == 250, "shown: slapback time spans 60 to 250 ms");
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
  char buf[8];
  formatShown(rate, 5, buf, 0);
  formatShown(rate, 15, buf, sizeof(buf));
  check(std::strcmp(buf, "1.5") == 0, "shown: formatShown takes any size and honours a size of zero");
}

// Every knob's widest readout, a number and its unit in 16X24, must fit the panel with 1 px each side.
void checkReadoutsFit() {
  const FontDesc& big = font16x24();
  bool fits = true;
  for (int c = 1; c < kCardCount; ++c) {
    for (int k = 0; k < cardDesc(card(c)).knobCount; ++k) {
      const KnobDesc& kd = *knobDesc(card(c), k);
      const int count = kd.kind == KnobKind::Stepped ? kd.positions : 101;
      for (int i = 0; i < count; ++i) {
        const float reading = kd.kind == KnobKind::Stepped ? static_cast<float>(i) : static_cast<float>(i) / 100.0f;
        char number[16];
        formatShown(kd, knobShown(kd, reading), number, sizeof(number));
        char unit[16];
        std::snprintf(unit, sizeof(unit), " %s", kd.unit);
        const int width = kd.labels ? Canvas::textWidth(big, number)
                                    : Canvas::digitsWidth(big, number) + big.spacing + Canvas::textWidth(big, unit);
        fits = fits && width <= kCanvasWidth - 2;
      }
    }
  }
  check(fits, "readout: every knob's widest value fits the panel in 16X24");
}

}  // namespace

void checkRegistry() {
  checkIds();
  checkCardTable();
  checkGroups();
  checkKnobsMove();
  checkKnobTargets();
  checkKnobClamps();
  checkRegistryBounds();
  checkDescriptors();
  checkChainDefaults();
  checkUnisonRate();
  checkToggles();
  checkPlacement();
  checkFormat();
  checkShownNumbers();
  checkShownLabels();
  checkShownDecimals();
  checkReadoutsFit();
}
