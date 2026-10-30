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
    case Card::Slapback: return knob == 0 ? p.slapback.mix : p.slapback.tuning.timeMs;
    case Card::Distortion: return knob == 0 ? p.distortion.drive : p.distortion.tone;
    case Card::ReverbSpring:
    case Card::ReverbChasm: return knob == 0 ? p.reverb.mix : p.reverb.time;
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
      check(knobShown(d, 0.0f) == std::lround(reversed ? d.max : d.min) && knobShown(d, 1.0f) == std::lround(reversed ? d.min : d.max),
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
  check(knobSteps(Card::Autotune, 0) == static_cast<int>(sizeof(cv::kKeyName) / sizeof(cv::kKeyName[0])),
        "key positions equal cv::kKeyName");
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
            std::fabs(t.holdMs - 50.0f) < 1e-6f && std::fabs(t.hysteresisDb - 3.0f) < 1e-6f &&
            std::fabs(t.attackMs - 0.5f) < 1e-6f && std::fabs(t.releaseMs - 200.0f) < 1e-6f &&
            std::fabs(t.detectorAttackMs - 0.1f) < 1e-6f && std::fabs(t.detectorReleaseMs - 5.0f) < 1e-6f &&
            t.detectorHpHz == 0.0f && t.travelTimes,
        "defaults: input gate on with the hard -80 dB range, 3 dB hysteresis and 50 ms hold");
  const cv::GateTuning& g = p.gate.tuning;
  check(g.kneeDb == cv::GateTuning{}.kneeDb && g.detectorAttackMs == 0.0f && g.detectorReleaseMs == 20.0f &&
            g.detectorHpHz == 120.0f && !g.travelTimes,
        "defaults: the panel gate keeps its soft-knee tuning");
  check(p.conditioning.hpf == cv::kHpfDefaultIndex && p.conditioning.lpf == cv::kLpfDefaultIndex &&
            cv::kHpfCutoffHz[p.conditioning.hpf] == 120.0f && cv::kLpfCutoffHz[p.conditioning.lpf] == 12000.0f,
        "defaults: the input conditioning starts at HPF 120 Hz and LPF 12 kHz");
  check(!p.gate.on && !p.pitchFx.autotune.on && !p.pitchFx.octave.on && !p.unison.on &&
            !p.slapback.on && !p.distortion.on && !p.reverb.on,
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
      {Card::Slapback, 1, 3},    // 60..250 ms
      {Card::Autotune, 1, 3},    // 5..500 ms response
      {Card::Distortion, 0, 3},  // 0..100 %
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
  checkShownDecimals();
  checkCells();
  checkFormatCells();
  checkCellsWiderLowEnd();
  checkCellsDecimals();
  checkReadoutsFit();
}
