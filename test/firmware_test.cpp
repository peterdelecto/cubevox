// Firmware core tier on the host: registry apply, descriptors, default layout, costs.
// Compiles firmware/cubevox/src/core/*.cpp; a core file that pulls in Arduino breaks this build.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/canvas.h"
#include "core/chain_params.h"
#include "core/compose.h"
#include "core/costs.h"
#include "core/display.h"
#include "core/fonts.h"
#include "core/jack_switch.h"
#include "core/knob_filter.h"
#include "core/layout.h"
#include "core/layout_fade.h"
#include "core/mute_sequence.h"
#include "core/panel.h"
#include "core/panel_test.h"
#include "core/protocol.h"
#include "core/tx_queue.h"
#include "core/record.h"
#include "core/record_store.h"
#include "core/registry.h"
#include "core/settings.h"
#include "core/version.h"
#include "ui/menu.h"

#include "cubevox_fonts.h"

namespace {

int gFailures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++gFailures;
  }
}

Card card(int i) { return static_cast<Card>(i); }

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
    case Card::Unison: return knob == 0 ? p.unison.depth : p.unisonRatePercent;
    case Card::Slapback: return knob == 0 ? p.slapback.intensity : p.slapback.tuning.timeMs;
    case Card::Distortion: return knob == 0 ? p.distortion.drive : p.distortion.tone;
    case Card::ReverbSpring:
    case Card::ReverbChasm: return knob == 0 ? p.reverb.intensity : p.reverb.time;
    case Card::Empty: break;
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
        applyKnob(c, k, s / 4.0f, p);
        const float v = engineValue(c, k, p);
        std::snprintf(what, sizeof(what), "%s knob %d engine value %d", cardDesc(c).id, k, s);
        if (s > 0) check(reversed ? v < prev : v > prev, what);
        if (!percent) check(std::fabs(v - knobValue(d, s / 4.0f)) < 1e-3f, what);
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
  applyKnob(Card::Unison, 1, 0.5f, p);
  check(std::fabs(p.unisonRatePercent - 50.0f) < 1e-4f, "unison rate 50 %");
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
  check(std::strcmp(buf, "+0 st") == 0, "semitones centre formats as +0 st");
  formatKnob(Card::Gate, 0, 1.0f, buf, sizeof(buf));
  check(std::strcmp(buf, "-10 dB") == 0, "gate threshold formats in dB");
  formatKnob(Card::Slapback, 1, 0.0f, buf, sizeof(buf));
  check(std::strcmp(buf, "60 ms") == 0, "slapback time formats in ms");
  formatKnob(Card::Distortion, 0, 0.5f, buf, sizeof(buf));
  check(std::strcmp(buf, "50 %") == 0, "drive formats in percent");
}

void checkDefaultLayout() {
  const Card expected[kSlotCount] = {Card::InputGate, Card::Autotune, Card::Octave, Card::Unison,
                                     Card::Slapback, Card::Distortion, Card::Gate, Card::ReverbSpring};
  int avg = costs::kFixed.avg;
  int worst = costs::kFixed.worst;
  int reverbs = 0;
  const Layout layout = defaultLayout();
  for (int s = 0; s < kSlotCount; ++s) {
    check(layout.card[s] == expected[s], "default layout slot");
    const CardDesc& d = cardDesc(layout.card[s]);
    avg += d.cost.avg;
    worst += d.cost.worst;
    if (d.group == Group::Reverb) ++reverbs;
  }
  check(avg <= costs::kBudgetAvgPercent, "default layout within the average line");
  check(worst <= costs::kBudgetWorstPercent, "default layout within the worst-block line");
  check(reverbs == 1, "default layout has one reverb");
  check(cardDesc(Card::Empty).cost.avg == 0 && cardDesc(Card::Empty).cost.worst == 0, "empty card costs nothing");
  for (int i = 0; i < kCardCount; ++i) {
    const CardDesc& d = cardDesc(card(i));
    check(d.cost.measured, "every card's cost comes from the board");
    check(d.cost.worst >= d.cost.avg, "worst block is never under the mean");
  }
}

void checkLayoutValid() {
  check(layoutValid(defaultLayout()).ok(), "default layout is valid");
  char why[96];
  Layout bad = defaultLayout();
  bad.card[3] = static_cast<Card>(kCardCount);
  LayoutCheck c = layoutValid(bad);
  layoutReason(c, why, sizeof why);
  check(!c.ok() && std::strcmp(why, "card id out of range in slot 4") == 0, "out-of-range card id is rejected");
  Layout twoReverbs = defaultLayout();
  twoReverbs.card[0] = Card::ReverbChasm;
  c = layoutValid(twoReverbs);
  layoutReason(c, why, sizeof why);
  check(!c.ok() && std::strcmp(why, "two reverb cards") == 0, "second reverb card is rejected");
  Layout twice = defaultLayout();
  twice.card[7] = twice.card[4];
  c = layoutValid(twice);
  layoutReason(c, why, sizeof why);
  check(!c.ok() && std::strcmp(why, "same card twice: slapback") == 0, "a card placed twice is rejected");
  Layout gaps = defaultLayout();
  gaps.card[0] = Card::Empty;
  gaps.card[1] = Card::Empty;
  check(layoutValid(gaps).ok(), "several Empty slots are accepted");

  LayoutCheck over;
  over.error = LayoutError::OverWorst;
  over.percent = 97;
  layoutReason(over, why, sizeof why);
  check(std::strcmp(why, "over CPU budget (worst 97 %)") == 0, "budget reason names the worst line");
  over.error = LayoutError::OverAvg;
  over.percent = 80;
  layoutReason(over, why, sizeof why);
  check(std::strcmp(why, "over CPU budget (mean 80 %)") == 0, "budget reason names the mean line");
}

PanelReading panelWithToggles(bool on) {
  PanelReading r;
  r.toggle.fill(on);
  return r;
}

void checkComposeToggles() {
  const Layout layout = defaultLayout();
  const ChainSnapshot off = composeSnapshot(layout, panelWithToggles(false), Settings(), BenchOverrides());
  check(!off.params.distortion.on && !off.params.unison.on && !off.params.reverb.on && !off.params.gate.on &&
            !off.params.pitchFx.autotune.on && !off.params.pitchFx.octave.on,
        "toggles off leave every stage off");
  const ChainSnapshot on = composeSnapshot(layout, panelWithToggles(true), Settings(), BenchOverrides());
  check(on.params.distortion.on && on.params.unison.on && on.params.reverb.on && on.params.gate.on &&
            on.params.pitchFx.autotune.on && on.params.pitchFx.octave.on,
        "toggles on turn every stage on");
  check(on.layout.card == layout.card, "snapshot carries the layout");
}

void checkComposeBench() {
  const Layout layout = defaultLayout();  // slot 6 (index 5) is Distortion
  BenchOverrides bench;
  bench.slotsOff = 1u << 5;
  ChainSnapshot snap = composeSnapshot(layout, panelWithToggles(true), Settings(), bench);
  check(!snap.params.distortion.on && snap.params.unison.on, "slotsOff bit forces only that slot off");

  bench = BenchOverrides();
  bench.reverbChasm = true;
  snap = composeSnapshot(layout, panelWithToggles(true), Settings(), bench);
  check(snap.params.reverb.engine == cv::kReverbChasm, "reverbChasm selects the chasm engine");

  // Octave semitones (stepped, slot index 2) and Unison depth (continuous, index 3) read the top position.
  PanelReading low;
  bench = BenchOverrides();
  bench.knobsPinned = true;
  const ChainSnapshot pinned = composeSnapshot(layout, low, Settings(), bench);
  ChainParams expect;
  applyKnob(Card::Octave, 0, 24.0f, expect);
  applyKnob(Card::Unison, 0, 1.0f, expect);
  check(pinned.params.pitchFx.octave.semitones == expect.pitchFx.octave.semitones &&
            pinned.params.pitchFx.octave.semitones == 12,
        "pinned stepped knob sits on its last step");
  check(std::fabs(pinned.params.unison.depth - expect.unison.depth) < 1e-6f, "pinned continuous knob sits at 1.0");
  const ChainSnapshot live = composeSnapshot(layout, low, Settings(), BenchOverrides());
  check(live.params.pitchFx.octave.semitones == -12, "unpinned stepped knob follows the panel");
}

void checkOutputGain() {
  Settings s;
  const BenchOverrides none;
  check(composeSnapshot(defaultLayout(), PanelReading(), s, none).outputGain == 1.0f, "LINE gain is unity");
  s.output = OutputLevel::Instrument;
  const float g = composeSnapshot(defaultLayout(), PanelReading(), s, none).outputGain;
  check(std::fabs(g - 0.2512f) < 1e-3f, "INSTRUMENT gain is -12 dB linear");
}

void checkSettings() {
  const Settings s;
  check(s.inputJack == InputJack::Mic, "input jack defaults to MIC");
  check(s.output == OutputLevel::Line, "output defaults to LINE");
  check(outputTrimDb(s) == 0.0f, "LINE output trim is 0 dB");
  Settings inst;
  inst.output = OutputLevel::Instrument;
  check(std::fabs(outputTrimDb(inst) - (-12.0f)) < 1e-6f, "INSTRUMENT output trim is -12 dB");
}

}  // namespace

void checkDebounce() {
  DebouncedInput in;
  in.reset(false);
  check(!in.update(true, 100, 20), "debounce: edge not reported at once");
  check(!in.update(true, 110, 20) && !in.level(), "debounce: no change before hold time");
  check(in.update(true, 120, 20) && in.level(), "debounce: changes after hold time");
  check(!in.update(true, 200, 20), "debounce: reported once");
  in.update(false, 300, 20);
  in.update(true, 310, 20);
  check(!in.update(true, 325, 20) && in.level(), "debounce: glitch shorter than hold ignored");
}

void checkSteppedPosition() {
  for (int p = 0; p < 4; ++p) {
    const float centre = (static_cast<float>(p) + 0.5f) / 4.0f;
    check(steppedPosition(centre, 4, 0) == p, "stepped: band centre");
  }
  check(steppedPosition(0.26f, 4, 0) == 0, "stepped: hysteresis holds just past the edge");
  check(steppedPosition(0.40f, 4, 0) == 1, "stepped: moves once clear of the hysteresis");
  check(steppedPosition(0.24f, 4, 1) == 1, "stepped: hysteresis holds just below the edge");
  check(steppedPosition(-0.5f, 4, 2) == 0, "stepped: clamps at 0");
  check(steppedPosition(1.0f, 4, 0) == 3 && steppedPosition(2.0f, 4, 0) == 3, "stepped: clamps at 1");
}

void settle(KnobFilter& f, float norm, int steps) {
  for (int i = 0; i < 60; ++i) f.update(norm, steps);
}

void checkKnobFilter() {
  KnobFilter f;
  check(!f.seeded(), "filter: unseeded at start");
  check(f.update(0.5f, 0) == KnobEvent::Seeded, "filter: first sample seeds");
  check(f.seeded() && std::fabs(f.value() - 0.5f) < 1e-6f, "filter: seed takes the sample");
  check(f.update(0.505f, 0) == KnobEvent::None, "filter: jitter under threshold is not a move");
  KnobEvent last = KnobEvent::None;
  bool moved = false;
  for (int i = 0; i < 20; ++i) {
    last = f.update(0.8f, 0);
    moved = moved || last == KnobEvent::Moved;
  }
  check(moved, "filter: real move reported");
  check(f.value() > 0.7f, "filter: value follows the knob");

  KnobFilter s;
  s.update(0.1f, 5);
  check(s.value() == 0.0f, "filter: stepped seeds at the step under the knob");
  bool stepMoved = false;
  for (int i = 0; i < 30; ++i) stepMoved = stepMoved || s.update(0.3f, 5) == KnobEvent::Moved;
  check(stepMoved && s.value() == 1.0f, "filter: stepped knob moves one step");
  check(s.update(0.3f, 5) == KnobEvent::None, "filter: stepped knob rests");
  settle(s, 0.95f, 5);
  check(s.update(0.95f, 10) == KnobEvent::Reseeded, "filter: step count change re-seeds");
  check(s.value() == 9.0f, "filter: re-seed lands on the position under the knob");
  check(s.update(0.95f, 0) == KnobEvent::Reseeded && s.value() < 1.0f, "filter: back to continuous re-seeds");
}

// Canvas page r holds text row r, so a row matches when its page equals the rendered text.
bool rowIs(const Canvas& canvas, int r, const char* text, bool inverted = false) {
  Canvas ref;
  ref.drawText(0, 0, font5x7(), text);
  for (int x = 0; x < kCanvasWidth; ++x) {
    const uint8_t want = inverted ? static_cast<uint8_t>(~ref.page(0)[x]) : ref.page(0)[x];
    if (canvas.page(r)[x] != want) return false;
  }
  return true;
}

std::string chainRow(const Layout& layout, int slot) {
  char line[kDisplayCols + 1];
  std::snprintf(line, sizeof(line), "%d %s", slot + 1, cardDesc(layout.card[slot]).name);
  return line;
}

bool pagesClean(const Canvas& canvas) { return canvas.dirtyPages() == 0; }

void clearAllDirty(Canvas& canvas) {
  for (int p = 0; p < kCanvasPages; ++p) canvas.clearDirty(p);
}

struct MenuRig {
  Menu menu;
  Layout layout = defaultLayout();
  PanelReading panel;
  Settings settings;
  Canvas canvas;
  uint32_t nowMs = 100;

  UiAction step(int detents, bool click, uint32_t advanceMs = 10) {
    nowMs += advanceMs;
    UiInput in;
    in.detents = detents;
    in.click = click;
    return menu.update(in, nowMs);
  }
  void draw() { menu.draw(UiContext{layout, panel, settings, false}, canvas); }
};

void checkMenu() {
  MenuRig m;
  m.draw();
  check(rowIs(m.canvas, 0, "cubevox"), "menu: idle title");
  char tag[kDisplayCols + 1];
  versionTag(kFirmwareVersion, tag, sizeof(tag));
  check(rowIs(m.canvas, 7, tag) && std::strchr(tag, '-') == nullptr, "menu: idle shows the version tag only");

  m.step(0, true);
  m.draw();
  check(rowIs(m.canvas, 0, "MENU"), "menu: click opens the list");
  check(rowIs(m.canvas, 1, ">Out LINE", true) && rowIs(m.canvas, 2, " Input MIC"), "menu: cursor 0 row inverted");
  check(rowIs(m.canvas, 2, " Input MIC"), "menu: input row text");

  m.step(-1, false);
  m.draw();
  check(rowIs(m.canvas, 5, ">Exit ", true), "menu: turning back from 0 wraps to the last item");
  m.step(1, false);
  m.draw();
  check(rowIs(m.canvas, 1, ">Out LINE", true), "menu: turning forward wraps to 0");

  check(m.step(0, true) == UiAction::ToggleOutput, "menu: Out click toggles output");
  m.step(1, false);
  check(m.step(0, true) == UiAction::ToggleInputJack, "menu: Input click toggles the jack");
  m.settings.inputJack = InputJack::Pedal;
  m.draw();
  check(rowIs(m.canvas, 2, ">Input PEDAL", true), "menu: row reads the settings");

  m.step(1, false);
  check(m.step(0, true) == UiAction::None, "menu: Chain order click has no action");
  m.draw();
  check(rowIs(m.canvas, 0, chainRow(m.layout, 0).c_str()), "menu: chain view starts at slot 1");
  check(rowIs(m.canvas, kDisplayRows - 1, chainRow(m.layout, 7).c_str()), "menu: chain view shows slot 8");
  m.step(0, true);
  m.draw();
  check(rowIs(m.canvas, 0, "MENU"), "menu: click leaves the chain view");

  m.step(1, false);
  m.step(0, true);
  m.draw();
  check(rowIs(m.canvas, 0, "UPDATE FIRMWARE"), "menu: confirm screen");
  m.step(1, false);
  m.draw();
  check(rowIs(m.canvas, 0, "MENU"), "menu: turn cancels the confirm");
  m.step(0, true);
  m.step(0, true);
  check(m.step(0, true) == UiAction::EnterDfu, "menu: click on confirm enters DFU");
}

void checkMenuTimeout() {
  MenuRig m;
  m.step(0, true);
  m.step(0, false, 14000);
  m.draw();
  check(rowIs(m.canvas, 0, "MENU"), "menu: still open before the timeout");
  m.step(0, false, 1500);
  m.draw();
  check(rowIs(m.canvas, 0, "cubevox"), "menu: 15 s without input returns to idle");
}

void checkMenuRedraw() {
  MenuRig m;
  m.draw();
  clearAllDirty(m.canvas);
  m.draw();
  check(pagesClean(m.canvas), "menu: an unchanged frame is not redrawn");
}

void checkMenuInvalidate() {
  MenuRig m;
  m.draw();
  clearAllDirty(m.canvas);
  m.menu.invalidate();
  m.draw();
  check(!pagesClean(m.canvas), "menu: invalidate makes the next draw repaint");
}

void checkCanvas() {
  namespace f = cubevox::fonts::StylizedHyperlegible5x7;
  const FontDesc& font = font5x7();
  const uint8_t* glyphA = &f::kGlyphs['A' - 0x20][0];

  Canvas c;
  check(c.dirtyPages() == 0xFF, "canvas: starts fully dirty");
  clearAllDirty(c);
  const int pen = c.drawText(0, 0, font, "A");
  check(std::memcmp(c.page(0), glyphA, f::kWidth) == 0, "canvas: 5x7 A matches the header bytes");
  check(c.page(0)[f::kWidth] == 0 && c.page(1)[0] == 0, "canvas: nothing past the glyph");
  check(c.dirtyPages() == 0x01, "canvas: drawing sets only the touched page dirty");
  check(pen == f::kWidth + f::kSpacing, "canvas: drawText returns the pen after the spacing");
  c.clearDirty(0);
  check(c.dirtyPages() == 0, "canvas: clearDirty clears the bit");

  Canvas shifted;
  shifted.drawText(0, 3, font, "A");
  bool shiftOk = true;
  for (int col = 0; col < f::kWidth; ++col) {
    shiftOk = shiftOk && shifted.page(0)[col] == static_cast<uint8_t>(glyphA[col] << 3);
    shiftOk = shiftOk && shifted.page(1)[col] == static_cast<uint8_t>(glyphA[col] >> 5);
  }
  check(shiftOk, "canvas: glyph at y = 3 splits across pages 0 and 1");

  Canvas above;
  above.drawText(0, -4, font, "A");
  bool aboveOk = true;
  for (int col = 0; col < f::kWidth; ++col) aboveOk = aboveOk && above.page(0)[col] == static_cast<uint8_t>(glyphA[col] >> 4);
  check(aboveOk, "canvas: glyph above the top edge clips to its lower rows");

  check(Canvas::textWidth(font, "AB") == 2 * (f::kWidth + f::kSpacing) - f::kSpacing, "canvas: textWidth drops the trailing gap");
  check(Canvas::textWidth(font, "") == 0, "canvas: textWidth of an empty string is 0");

  Canvas edge;
  edge.drawText(125, 60, font, "AB");
  edge.drawText(-3, -3, font, "A");
  edge.drawText(120, 62, font, "AAA");
  edge.fillRect(-5, -5, 300, 300, true);
  edge.invertRect(100, 50, 100, 100);
  edge.setPixel(-1, 0, true);
  edge.setPixel(128, 64, true);
  check(edge.page(7)[127] == 0x00 && edge.page(6)[127] == 0x03 && edge.page(0)[0] == 0xFF, "canvas: fill and invert clip at the edges");

  // A capitals-only font, so lowercase has no glyph of its own.
  const uint8_t caps[2][5] = {{1, 2, 3, 4, 5}, {6, 7, 8, 9, 10}};
  const FontDesc capsOnly{5, 7, 1, 5, "AB", 2, &caps[0][0], false};
  Canvas low;
  Canvas up;
  low.drawText(0, 0, capsOnly, "ab");
  up.drawText(0, 0, capsOnly, "AB");
  check(low.page(0)[0] == 1 && std::memcmp(low.page(0), up.page(0), kCanvasWidth) == 0, "canvas: lowercase falls back to uppercase");
  Canvas gap;
  gap.drawText(0, 0, capsOnly, "?B");
  Canvas direct;
  direct.drawText(capsOnly.width + capsOnly.spacing, 0, capsOnly, "B");
  check(std::memcmp(gap.page(0), direct.page(0), kCanvasWidth) == 0, "canvas: a missing glyph is blank and keeps its advance");

  Canvas lower5;
  Canvas upper5;
  lower5.drawText(0, 0, font, "u");
  upper5.drawText(0, 0, font, "U");
  check(lower5.page(0)[1] != 0 && std::memcmp(lower5.page(0), upper5.page(0), kCanvasWidth) == 0,
        "canvas: 5x7 u draws the bytes of U");

  namespace f9 = cubevox::fonts::StylizedHyperlegible9X12;
  Canvas big9;
  big9.drawText(0, 0, font9x12(), "u");
  bool ok9 = true;
  for (int col = 0; col < f9::kWidth; ++col) {
    ok9 = ok9 && big9.page(0)[col] == f9::kGlyphs['U' - 0x20][col];
    ok9 = ok9 && big9.page(1)[col] == (f9::kGlyphs['U' - 0x20][f9::kWidth + col] & 0x0F);
  }
  check(ok9 && big9.page(0)[1] != 0, "canvas: 9x12 u draws the glyph bytes of U");

  namespace f16 = cubevox::fonts::StylizedHyperlegible16X24;
  Canvas big16;
  big16.drawText(0, 0, font16x24(), "H");
  bool ok16 = true;
  for (int col = 0; col < f16::kWidth; ++col)
    for (int band = 0; band < 3; ++band)
      ok16 = ok16 && big16.page(band)[col] == f16::kGlyphs['H' - 0x20][band * f16::kWidth + col];
  check(ok16 && big16.page(0)[0] != 0, "canvas: 16x24 H draws the glyph bytes");

  Canvas rect;
  clearAllDirty(rect);
  rect.fillRect(10, 10, 4, 3, true);
  check(rect.dirtyPages() == 0x02 && rect.page(1)[10] == 0x1C, "canvas: fillRect sets the rows inside page 1");
  rect.invertRect(10, 10, 4, 3);
  check(rect.page(1)[10] == 0 && rect.page(1)[13] == 0, "canvas: invertRect undoes a fill");
}

// Writes a screen as a binary PGM at 4x, lit pixels white.
void writePgm(const Canvas& canvas, const char* name) {
  constexpr int kScale = 4;
  const std::string path = std::string(CUBEVOX_OUT_DIR) + "/" + name;
  std::FILE* file = std::fopen(path.c_str(), "wb");
  check(file != nullptr, "pgm: output file opens");
  if (!file) return;
  std::fprintf(file, "P5\n%d %d\n255\n", kCanvasWidth * kScale, kCanvasHeight * kScale);
  for (int y = 0; y < kCanvasHeight * kScale; ++y) {
    const int py = y / kScale;
    for (int x = 0; x < kCanvasWidth * kScale; ++x) {
      const bool on = (canvas.page(py / 8)[x / kScale] >> (py % 8)) & 1;
      std::fputc(on ? 0xFF : 0x00, file);
    }
  }
  std::fclose(file);
}

void checkScreenshots() {
  MenuRig m;
  m.panel.lastMoved.slot = 5;
  m.panel.knob[5][0] = 0.6f;
  m.panel.toggle[5] = true;
  m.draw();
  writePgm(m.canvas, "oled-idle.pgm");

  m.step(0, true);
  m.draw();
  writePgm(m.canvas, "oled-menu.pgm");

  m.step(2, false);
  m.step(0, true);
  m.draw();
  writePgm(m.canvas, "oled-chain.pgm");
}

void checkJackSwitch() {
  JackSwitch j;
  j.update(0, InputJack::Mic, false);
  check(!j.holdMute() && j.relay() == InputJack::Mic, "jack: no change, no hold");

  j.update(10, InputJack::Pedal, false);
  check(j.holdMute() && j.relay() == InputJack::Mic, "jack: request holds the mute, relay waits");
  j.update(20, InputJack::Pedal, false);
  check(j.relay() == InputJack::Mic, "jack: relay waits until fully muted");
  j.update(30, InputJack::Pedal, true);
  check(j.relay() == InputJack::Pedal && j.holdMute(), "jack: relay switches when fully muted");
  j.update(30 + kRelaySettleMs - 1, InputJack::Pedal, true);
  check(j.holdMute(), "jack: hold stays until the relay settles");
  j.update(30 + kRelaySettleMs, InputJack::Pedal, true);
  check(!j.holdMute() && j.relay() == InputJack::Pedal, "jack: hold released after the settle time");

  j.update(100, InputJack::Mic, true);
  j.update(110, InputJack::Pedal, true);
  check(j.relay() == InputJack::Mic, "jack: relay moved on the first request");
  j.update(111, InputJack::Pedal, true);
  j.update(111 + kRelaySettleMs, InputJack::Pedal, true);
  j.update(112 + kRelaySettleMs, InputJack::Pedal, true);
  check(j.holdMute() && j.relay() == InputJack::Pedal, "jack: a second change mid-sequence ends on the latest request");
  j.update(112 + 2 * kRelaySettleMs, InputJack::Pedal, true);
  check(!j.holdMute() && j.relay() == InputJack::Pedal, "jack: latest request settles and releases");
}

void checkVersionTag() {
  char buf[16];
  versionTag("v2.00.04-3-gabc123-dirty", buf, sizeof(buf));
  check(std::strcmp(buf, "v2.00.04") == 0, "version: tag is cut at the first dash");
  versionTag("v2.00.04", buf, sizeof(buf));
  check(std::strcmp(buf, "v2.00.04") == 0, "version: a bare tag passes through");
  versionTag("dev", buf, sizeof(buf));
  check(std::strcmp(buf, "dev") == 0, "version: dev passes through");
  char small[5];
  versionTag("v2.00.04-1-gabc", small, sizeof(small));
  check(std::strcmp(small, "v2.0") == 0, "version: clipped to the buffer");
}

// Steps the sequence once per ms from `from` to `to` inclusive and returns the last outputs.
MuteOutputs runMute(MuteSequence& m, uint32_t from, uint32_t to, bool released) {
  MuteOutputs out;
  for (uint32_t t = from; t <= to; ++t) out = m.update(t, released);
  return out;
}

void checkMuteSequence() {
  MuteSequence m;
  m.reset(true, 0);
  MuteOutputs out = runMute(m, 0, kBootUnmuteDelayMs - 1, true);
  check(!out.xsmtHigh && !out.muteNHigh && m.fullyMuted(), "mute: stays muted before the clocks run");
  m.clocksRunning(100);
  out = runMute(m, 100, 100 + kBootUnmuteDelayMs - 1, true);
  check(!out.xsmtHigh && !out.muteNHigh, "mute: boot delay holds the output muted");

  uint32_t t = 100 + kBootUnmuteDelayMs;
  out = m.update(t, true);
  check(out.muteNHigh && !out.xsmtHigh && !m.fullyMuted(), "mute: release raises MUTE_N first");
  out = runMute(m, t + 1, t + kRelaySettleMs - 1, true);
  check(out.muteNHigh && !out.xsmtHigh, "mute: XSMT waits for the relay to settle");
  out = m.update(t + kRelaySettleMs, true);
  check(out.muteNHigh && out.xsmtHigh && !m.active(), "mute: XSMT rises after the settle");

  t += kRelaySettleMs + 100;
  runMute(m, t, t + kMuteSwitchDebounceMs - 1, false);
  out = m.update(t + kMuteSwitchDebounceMs, false);
  check(!out.xsmtHigh && out.muteNHigh, "mute: XSMT falls first, MUTE_N still high");
  out = runMute(m, t + kMuteSwitchDebounceMs + 1, t + kMuteSwitchDebounceMs + kXsmtLeadMs - 1, false);
  check(!out.xsmtHigh && out.muteNHigh && !m.fullyMuted(), "mute: MUTE_N waits out the XSMT lead");
  out = m.update(t + kMuteSwitchDebounceMs + kXsmtLeadMs, false);
  check(!out.xsmtHigh && !out.muteNHigh && m.fullyMuted(), "mute: MUTE_N falls after the lead");
}

void checkMuteLatchAndHold() {
  MuteSequence m;
  m.reset(true, 0);
  m.clocksRunning(0);
  runMute(m, 0, kBootUnmuteDelayMs + kRelaySettleMs + 1, true);
  check(!m.active(), "mute: unmuted after the boot delay and settle");

  uint32_t t = 1000;
  m.hold(true);
  MuteOutputs out = runMute(m, t, t + kXsmtLeadMs, true);
  check(!out.xsmtHigh && !out.muteNHigh && m.fullyMuted(), "mute: a hold mutes through the sequence");
  t += 100;
  out = runMute(m, t, t + kRelaySettleMs + 1, true);
  check(!out.xsmtHigh && !out.muteNHigh, "mute: the output stays muted while held");
  m.hold(false);
  t += 100;
  out = runMute(m, t, t + kRelaySettleMs + 1, true);
  check(out.xsmtHigh && out.muteNHigh, "mute: releasing the hold runs the release sequence");

  t += 100;
  m.latch();
  out = runMute(m, t, t + kXsmtLeadMs, true);
  check(m.fullyMuted() && !out.xsmtHigh, "mute: a latch mutes");
  out = runMute(m, t + 100, t + 300, true);
  check(!out.xsmtHigh && !out.muteNHigh, "mute: a latch ignores a released switch");

  MuteSequence mid;
  mid.reset(true, 0);
  mid.clocksRunning(0);
  runMute(mid, 0, kBootUnmuteDelayMs, true);
  mid.hold(true);
  out = mid.update(kBootUnmuteDelayMs + 1, true);
  check(!out.xsmtHigh && out.muteNHigh, "mute: muted again mid-release keeps XSMT low");
  out = runMute(mid, kBootUnmuteDelayMs + 2, kBootUnmuteDelayMs + kXsmtLeadMs + 2, true);
  check(!out.muteNHigh && mid.fullyMuted(), "mute: a mid-release mute ends fully muted");
}


// In-memory bank 2: two 128 KB sectors, 0xFF erased. Programming ANDs bits in like real flash.
class FakeFlash : public FlashIo {
 public:
  FakeFlash() { mem[0].assign(kRecordSectorBytes, 0xFF); mem[1].assign(kRecordSectorBytes, 0xFF); }
  const uint8_t* read(int s) const override { return mem[s].data(); }
  void eraseStart(int s) override { erasing = s; busyPolls = 3; ++erases[s]; ++ops; }
  bool eraseBusy() override {
    if (erasing < 0) return false;
    if (busyPolls-- > 0) return true;
    mem[erasing].assign(kRecordSectorBytes, 0xFF);
    erasing = -1;
    return false;
  }
  void programWord(int s, uint32_t off, const uint8_t* w) override {
    for (size_t i = 0; i < kFlashWordBytes; ++i) mem[s][off + i] &= w[i];
    ++programs;
    ++ops;
  }
  bool takeError() override { return false; }

  void put(int sector, int slot, const RecordData& rec) {
    const RecordBytes b = encodeRecord(rec);
    std::memcpy(&mem[sector][slot * kRecordBytes], b.data(), kRecordBytes);
  }

  std::vector<uint8_t> mem[2];
  int erasing = -1;
  int busyPolls = 0;
  int erases[2] = {0, 0};
  int programs = 0;
  int ops = 0;
};

RecordData makeRec(uint32_t seq, OutputLevel out = OutputLevel::Line, const char* name = "") {
  RecordData r;
  r.sequence = seq;
  r.settings.output = out;
  std::strncpy(r.name, name, kRecordNameBytes);
  return r;
}

void checkRecordCodec() {
  const uint8_t vec[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  check(crc32(vec, sizeof vec) == 0xCBF43926u, "record: crc32 of 123456789");

  RecordData in = makeRec(77, OutputLevel::Instrument, "stage rig");
  in.settings.inputJack = InputJack::Pedal;
  in.layout.card[2] = Card::ReverbChasm;
  in.layout.card[7] = Card::Empty;
  RecordBytes b = encodeRecord(in);
  RecordData out;
  check(decodeRecord(b.data(), out), "record: round trip decodes");
  check(out.sequence == 77 && out.settings.output == OutputLevel::Instrument &&
            out.settings.inputJack == InputJack::Pedal && std::strcmp(out.name, "stage rig") == 0 &&
            out.layout.card == in.layout.card,
        "record: round trip keeps every field");

  RecordBytes bad = b;
  bad[kOffName] ^= 0x01;
  check(!decodeRecord(bad.data(), out), "record: corrupt byte fails the CRC");
  bad = b;
  bad[kOffSchema] = 2;
  check(!decodeRecord(bad.data(), out), "record: wrong schema rejected");

  bad = b;
  std::memset(&bad[kOffSlots], 0, kCardIdBytes);
  std::memcpy(&bad[kOffSlots], "no_such_card", 12);
  const uint32_t crc = crc32(bad.data(), kOffCrc);
  for (int i = 0; i < 4; ++i) bad[kOffCrc + i] = static_cast<uint8_t>(crc >> (8 * i));
  check(decodeRecord(bad.data(), out) && out.layout.card[0] == Card::Empty, "record: unknown card id becomes empty");

  const uint8_t blank[kRecordBytes] = {};
  std::vector<uint8_t> ff(kRecordBytes, 0xFF);
  check(classifyRecordSlot(ff.data()) == SlotState::Blank, "record: all 0xFF is blank");
  check(classifyRecordSlot(blank) == SlotState::Torn, "record: zeros are torn");
  check(classifyRecordSlot(b.data()) == SlotState::Valid, "record: encoded slot is valid");
}

void checkRecordNewest() {
  FakeFlash f;
  f.put(0, 0, makeRec(3));
  f.put(1, 0, makeRec(5, OutputLevel::Instrument));
  f.put(0, 1, makeRec(4));
  const uint8_t* views[2] = {f.read(0), f.read(1)};
  RecordFound found;
  check(newestRecord(views, 2, kRecordSectorBytes, found) && found.rec.sequence == 5 && found.sector == 1,
        "record: newest wins across sectors");

  RecordStore store(f);
  RecordData out;
  check(store.begin(out) && out.sequence == 5, "store: boot loads the newest");
  check(store.status().sector == 1 && store.status().nextSlot == 1, "store: append goes after the newest");
}

// Polls until the store is idle; returns the largest number of flash ops one poll started.
int runToIdle(RecordStore& store, FakeFlash& f, uint32_t& t) {
  int worst = 0;
  for (int i = 0; i < 100 && store.status().busy; ++i) {
    const int before = f.ops;
    store.poll(t++);
    worst = std::max(worst, f.ops - before);
  }
  return worst;
}

void checkStoreAppend() {
  FakeFlash f;
  RecordStore store(f);
  RecordData out;
  check(!store.begin(out), "store: blank flash has no record");

  Settings s;
  s.output = OutputLevel::Instrument;
  store.request(defaultLayout(), s, 100);
  store.poll(2099);
  check(f.ops == 0, "store: no write before 2 s of quiet");
  store.request(defaultLayout(), s, 1500);
  store.poll(3499);
  check(f.ops == 0, "store: a new request restarts the debounce");
  uint32_t t = 3500;
  const int worst = runToIdle(store, f, t);
  check(f.programs == kWordsPerRecord && f.erases[0] + f.erases[1] == 0, "store: one record is eight word programs, no erase");
  check(worst == 1, "store: at most one flash op per poll");

  FakeFlash again;
  std::memcpy(again.mem[0].data(), f.mem[0].data(), kRecordBytes);
  RecordStore reboot(again);
  check(reboot.begin(out) && out.sequence == 1 && out.settings.output == OutputLevel::Instrument,
        "store: the saved record reloads");

  store.saveNow();
  store.poll(3501 + 1000);
  check(f.programs == kWordsPerRecord, "store: save-now starts without waiting (first poll sets up)");
  runToIdle(store, f, t);
  check(f.programs == 2 * kWordsPerRecord && store.status().sequence == 2 && store.status().nextSlot == 2,
        "store: second record appends after the first");
}

void checkStoreTornTail() {
  FakeFlash f;
  f.put(0, 0, makeRec(1));
  f.put(0, 1, makeRec(2, OutputLevel::Instrument));
  f.put(0, 2, makeRec(3));
  std::memset(&f.mem[0][2 * kRecordBytes + 3 * kFlashWordBytes], 0xFF, 5 * kFlashWordBytes);  // cut after 3 words
  f.mem[0][2 * kRecordBytes + 100] = 0x00;                                                        // and damaged
  RecordStore store(f);
  RecordData out;
  check(store.begin(out) && out.sequence == 2, "store: torn tail is skipped, previous record loads");
  check(store.status().nextSlot == 3, "store: append goes after the torn slot");
  store.request(defaultLayout(), Settings{}, 0);
  uint32_t t = kSaveDebounceMs;
  runToIdle(store, f, t);
  const uint8_t* views[2] = {f.read(0), f.read(1)};
  RecordFound found;
  check(newestRecord(views, 2, kRecordSectorBytes, found) && found.rec.sequence == 3 && found.slot == 3,
        "store: record lands in slot 3 with sequence 3");
}

void fillSector(FakeFlash& f, int sector, uint32_t firstSeq) {
  for (int slot = 0; slot < kSlotsPerSector; ++slot) f.put(sector, slot, makeRec(firstSeq + slot));
}

void checkStoreSectorSwitch() {
  FakeFlash f;
  fillSector(f, 0, 1);
  f.put(1, 0, makeRec(0));
  std::memset(&f.mem[1][kRecordBytes], 0x5A, 64);  // stale junk in the other sector
  RecordStore store(f);
  RecordData out;
  check(store.begin(out) && out.sequence == kSlotsPerSector, "store: full sector loads its last record");
  store.request(defaultLayout(), Settings{}, 0);
  uint32_t t = kSaveDebounceMs;
  const int worst = runToIdle(store, f, t);
  check(f.erases[1] == 1 && f.erases[0] == 0, "store: full sector erases the other one");
  check(worst == 1, "store: erase start and programs are separate polls");
  check(store.status().sector == 1 && store.status().nextSlot == 1 && store.status().sequence == kSlotsPerSector + 1,
        "store: new record is slot 0 of the other sector");
  const uint8_t* views[2] = {f.read(0), f.read(1)};
  RecordFound found;
  check(newestRecord(views, 2, kRecordSectorBytes, found) && found.sector == 1 && found.slot == 0,
        "store: newest is in the fresh sector");
}

void checkStoreInterruptedErase() {
  // The older sector was mid-erase: a blank slot followed by old data.
  FakeFlash f;
  fillSector(f, 1, 1);
  std::memset(&f.mem[1][0], 0xFF, 3 * kRecordBytes);
  fillSector(f, 0, 600);
  RecordStore store(f);
  RecordData out;
  check(store.begin(out) && out.sequence == 600 + kSlotsPerSector - 1, "store: dirty sector does not hide the newest");
  store.request(defaultLayout(), Settings{}, 0);
  uint32_t t = kSaveDebounceMs;
  runToIdle(store, f, t);
  check(f.erases[1] == 1, "store: interrupted erase is re-erased before use");

  // No valid record anywhere and sector 0 dirty: use the blank sector, leave the dirty one.
  FakeFlash g;
  std::memset(&g.mem[0][kRecordBytes], 0x33, 64);
  RecordStore fresh(g);
  check(!fresh.begin(out), "store: junk is not a record");
  fresh.request(defaultLayout(), Settings{}, 0);
  t = kSaveDebounceMs;
  runToIdle(fresh, g, t);
  check(g.erases[0] + g.erases[1] == 0 && fresh.status().sector == 1 && fresh.status().sequence == 1,
        "store: a blank sector is used without an erase");

  // Bench erase wipes both.
  check(fresh.eraseAll(), "store: erase-all accepted when idle");
  for (int i = 0; i < 50; ++i) fresh.poll(t++);
  check(g.erases[0] == 1 && g.erases[1] == 1 && !fresh.status().hasRecord && !fresh.status().busy,
        "store: erase-all clears both sectors");
}

bool pixelOn(const Canvas& c, int x, int y) { return (c.page(y / 8)[x] >> (y % 8)) & 1; }

void checkPanelTestPots() {
  PanelTest t;
  t.reset();
  check(!t.updatePot(3, 0.5f, 0), "panel test: the first sample is not movement");
  check(t.updatePot(3, 0.6f, 100), "panel test: a step past the print threshold reports");
  check(!t.updatePot(3, 0.61f, 200), "panel test: a small move stays quiet");
  check(!t.updatePot(3, 0.7f, 105), "panel test: prints are rate limited per pot");
  check(t.updatePot(3, 0.7f, 150), "panel test: a held-back move reports once the gap passes");
  check(t.updatePot(3, 0.02f, 300), "panel test: reaching the low end reports");
  check(!t.potDone(3), "panel test: one end is not a full travel");
  check(t.updatePot(3, 0.99f, 400), "panel test: reaching the high end reports");
  check(t.potDone(3) && t.potMin(3) < 0.05f && t.potMax(3) > 0.95f, "panel test: both ends make a full travel");
  check(t.nextPot() == 0, "panel test: next untested pot is the first one not done");
  for (int i = 0; i < kPanelTestPots; ++i) {
    t.updatePot(i, 0.0f, 0);
    t.updatePot(i, 1.0f, 1000);
  }
  check(t.potsDone() == kPanelTestPots && t.nextPot() == -1, "panel test: all pots done");
  t.reset();
  check(t.potsDone() == 0, "panel test: reset forgets the travel");
}

void checkPanelTestToggles() {
  PanelTest t;
  t.reset();
  check(!t.updateToggle(2, true), "panel test: the first toggle sample is not an edge");
  check(!t.toggleDone(2), "panel test: one state is not both");
  check(!t.updateToggle(2, true), "panel test: a steady toggle has no edge");
  check(t.updateToggle(2, false), "panel test: a toggle change is an edge");
  check(t.toggleDone(2) && t.nextToggle() == 0, "panel test: both states seen, next toggle is slot 1");
  for (int s = 0; s < kPanelTestToggles; ++s) {
    t.updateToggle(s, true);
    t.updateToggle(s, false);
  }
  check(t.togglesDone() == kPanelTestToggles && t.nextToggle() == -1, "panel test: all toggles done");
}

void checkPanelTestPattern() {
  Canvas c;
  drawPanelTestPattern(c);
  bool border = true;
  for (int x = 0; x < kCanvasWidth; ++x) border = border && pixelOn(c, x, 0) && pixelOn(c, x, 63);
  for (int y = 0; y < kCanvasHeight; ++y) border = border && pixelOn(c, 0, y) && pixelOn(c, 127, y);
  check(border, "panel pattern: 1 px border");
  check(pixelOn(c, 100, 4) && !pixelOn(c, 104, 4) && pixelOn(c, 104, 8), "panel pattern: checkerboard cells alternate");
  auto inked = [&](int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x)
        if (pixelOn(c, x, y)) return true;
    return false;
  };
  check(inked(4, 4, 40, 11), "panel pattern: 5x7 line");
  check(inked(4, 16, 40, 28), "panel pattern: 9x12 line");
  check(inked(4, 34, 90, 58), "panel pattern: 16x24 line");
  check(!inked(60, 59, 120, 63), "panel pattern: nothing below the text");
}

void checkPanelTest() {
  checkPanelTestPots();
  checkPanelTestToggles();
  checkPanelTestPattern();
}

void checkStoreName() {
  FakeFlash f;
  RecordStore store(f);
  RecordData out;
  store.begin(out);
  store.setName("stage rig");
  store.request(defaultLayout(), Settings{}, 0);
  uint32_t t = 2000;
  runToIdle(store, f, t);
  RecordData back;
  check(decodeRecord(f.mem[0].data(), back) && std::strcmp(back.name, "stage rig") == 0, "store: the layout name is saved");
  store.setName("a name that is longer than thirty-two bytes");
  check(std::strlen(store.current().name) == kRecordNameBytes, "store: a long name is clipped to 32 bytes");
}

void checkRecords() {
  checkRecordCodec();
  checkRecordNewest();
  checkStoreAppend();
  checkStoreName();
  checkStoreTornTail();
  checkStoreSectorSwitch();
  checkStoreInterruptedErase();
}

// ---- serial protocol ------------------------------------------------------------------------

class StringWriter : public protocol::Writer {
 public:
  void write(const char* text, size_t len) override {
    out.append(text, len);
    ++calls;
  }
  std::string out;
  int calls = 0;
};

// Strict JSON reader: true when `s` is one whole valid value. Enough to prove the replies parse.
class JsonReader {
 public:
  explicit JsonReader(const std::string& s) : s_(s) {}
  bool valid() {
    ws();
    return value(0) && (ws(), i_ == s_.size() || (s_[i_] == '\n' && i_ + 1 == s_.size()));
  }

 private:
  void ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r')) ++i_; }
  bool lit(const char* w) {
    const size_t n = std::strlen(w);
    if (s_.compare(i_, n, w) != 0) return false;
    i_ += n;
    return true;
  }
  bool str() {
    if (i_ >= s_.size() || s_[i_] != '"') return false;
    for (++i_; i_ < s_.size(); ++i_) {
      const unsigned char c = static_cast<unsigned char>(s_[i_]);
      if (c == '"') { ++i_; return true; }
      if (c < 0x20) return false;
      if (c == '\\') {
        ++i_;
        if (i_ >= s_.size() || !std::strchr("\"\\/bfnrtu", s_[i_])) return false;
        if (s_[i_] == 'u') {
          for (int k = 0; k < 4; ++k) {
            if (++i_ >= s_.size() || !std::isxdigit(static_cast<unsigned char>(s_[i_]))) return false;
          }
        }
      }
    }
    return false;
  }
  bool num() {
    const size_t start = i_;
    while (i_ < s_.size() && std::strchr("+-0123456789.eE", s_[i_])) ++i_;
    return i_ > start;
  }
  bool seq(char close, bool pairs, int depth) {
    ++i_;
    ws();
    if (i_ < s_.size() && s_[i_] == close) { ++i_; return true; }
    for (;;) {
      ws();
      if (pairs && (!str() || (ws(), i_ >= s_.size() || s_[i_++] != ':'))) return false;
      if (!value(depth + 1)) return false;
      ws();
      if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
      if (i_ < s_.size() && s_[i_] == close) { ++i_; return true; }
      return false;
    }
  }
  bool value(int depth) {
    ws();
    if (depth > 8 || i_ >= s_.size()) return false;
    switch (s_[i_]) {
      case '{': return seq('}', true, depth);
      case '[': return seq(']', false, depth);
      case '"': return str();
      case 't': return lit("true");
      case 'f': return lit("false");
      case 'n': return lit("null");
      default: return num();
    }
  }
  const std::string& s_;
  size_t i_ = 0;
};

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

bool parses(const char* line, protocol::Request& req, std::string& err) {
  char buf[protocol::kErrorBytes];
  const bool ok = protocol::parseRequest(line, req, buf, sizeof buf);
  err = ok ? "" : buf;
  return ok;
}

void checkProtocolCommands() {
  protocol::Request r;
  std::string err;
  check(parses("{\"id\":7,\"cmd\":\"hello\"}", r, err) && r.id == 7 && r.cmd == protocol::Command::Hello, "proto: hello parses");
  check(parses("{\"cmd\":\"get_layout\",\"id\":8}", r, err) && r.id == 8 && r.cmd == protocol::Command::GetLayout,
        "proto: key order is free");
  check(parses(" { \"id\" : 9 , \"cmd\" : \"status\" } ", r, err) && r.cmd == protocol::Command::Status, "proto: whitespace is free");
  check(parses("{\"id\":10,\"cmd\":\"enter_dfu\"}", r, err) && r.cmd == protocol::Command::EnterDfu, "proto: enter_dfu parses");
  check(parses("{\"id\":4294967295,\"cmd\":\"hello\"}", r, err) && r.id == 4294967295u, "proto: largest id parses");
  check(parses("{\"id\":1,\"cmd\":\"hello\",\"x\":[1,{\"a\":[true,null,\"s\\\"\"]},-2.5e3],\"y\":{}}", r, err),
        "proto: unknown keys with nested values are skipped");

  const char* set =
      "{\"id\":3,\"cmd\":\"set_layout\",\"name\":\"Stage \\u0041\\\"x\",\"slots\":[\"input_gate\",\"autotune\","
      "\"octave\",\"unison\",\"slapback\",\"distortion\",\"gate\",\"reverb_chasm\"]}";
  check(parses(set, r, err) && r.cmd == protocol::Command::SetLayout && r.slotCount == 8, "proto: set_layout parses");
  check(std::strcmp(r.name, "Stage A\"x") == 0, "proto: escapes in the name decode");
  Layout l;
  check(protocol::resolveLayout(r, l, nullptr, 0) && l.card[7] == Card::ReverbChasm && l.card[0] == Card::InputGate,
        "proto: set_layout resolves to cards");
}

void checkProtocolErrors() {
  protocol::Request r;
  std::string err;
  check(!parses("{\"id\":1,\"cmd\":\"hello\"", r, err) && err == "malformed json", "proto: unterminated object");
  check(!parses("not json", r, err) && err == "malformed json", "proto: not json");
  check(!parses("{\"id\":1,\"cmd\":\"hello\"} x", r, err) && err == "malformed json", "proto: trailing text");
  check(!parses("{\"id\":1,}", r, err) && err == "malformed json", "proto: trailing comma");
  check(!parses("{\"id\":5,\"cmd\":\"hel", r, err) && r.id == 5, "proto: a cut-off line still echoes the id it read");
  check(!parses("{\"cmd\":\"hello\"}", r, err) && err == "missing id" && r.id == 0, "proto: missing id");
  check(!parses("{\"id\":\"1\",\"cmd\":\"hello\"}", r, err) && err == "id must be a non-negative integer", "proto: string id");
  check(!parses("{\"id\":-1,\"cmd\":\"hello\"}", r, err) && err == "id must be a non-negative integer", "proto: negative id");
  check(!parses("{\"id\":1.5,\"cmd\":\"hello\"}", r, err) && err == "id must be a non-negative integer", "proto: fractional id");
  check(!parses("{\"id\":4294967296,\"cmd\":\"hello\"}", r, err) && err == "id must be a non-negative integer", "proto: id over 32 bits");
  check(!parses("{\"id\":2}", r, err) && err == "missing cmd" && r.id == 2, "proto: missing cmd echoes the id");
  check(!parses("{\"id\":2,\"cmd\":4}", r, err) && err == "cmd must be a string", "proto: numeric cmd");
  check(!parses("{\"id\":2,\"cmd\":\"fly\"}", r, err) && err == "unknown cmd \"fly\"" && r.id == 2, "proto: unknown cmd");
  check(!parses("{\"id\":2,\"cmd\":\"set_layout\",\"slots\":[]}", r, err) && err == "missing name", "proto: set_layout needs a name");
  check(!parses("{\"id\":2,\"cmd\":\"set_layout\",\"name\":\"a\"}", r, err) && err == "missing slots", "proto: set_layout needs slots");
  check(!parses("{\"id\":2,\"cmd\":\"set_layout\",\"name\":3,\"slots\":[]}", r, err) && err == "name must be a string",
        "proto: numeric name");
  check(!parses("{\"id\":2,\"cmd\":\"set_layout\",\"name\":\"a\",\"slots\":\"x\"}", r, err) &&
            err == "slots must be an array of card ids", "proto: slots not an array");
  check(!parses("{\"id\":2,\"cmd\":\"set_layout\",\"name\":\"a\",\"slots\":[1,2,3,4,5,6,7,8]}", r, err) &&
            err == "slots must be an array of card ids", "proto: slot ids must be strings");
  check(!parses("{\"id\":2,\"cmd\":\"set_layout\",\"name\":\"a\",\"slots\":[\"gate\"]}", r, err) &&
            err == "slots needs 8 card ids", "proto: too few slots");
  check(!parses("{\"id\":2,\"cmd\":\"set_layout\",\"name\":\"a\",\"slots\":[\"a\",\"a\",\"a\",\"a\",\"a\",\"a\",\"a\",\"a\",\"a\"]}",
                r, err) && err == "slots needs 8 card ids", "proto: too many slots");
  const std::string nameRule = "name must be 1 to 31 printable ASCII characters";
  const std::string head = "{\"id\":2,\"cmd\":\"set_layout\",\"name\":\"";
  const std::string tailSlots = "\",\"slots\":[]}";
  check(!parses((head + "0123456789012345678901234567890" + "1" + tailSlots).c_str(), r, err) && err == nameRule,
        "proto: name of 32 characters is rejected");
  check(!parses((head + "0123456789012345678901234567890" + tailSlots).c_str(), r, err) && err == "slots needs 8 card ids",
        "proto: name of 31 characters passes the name rule");
  check(!parses((head + tailSlots).c_str(), r, err) && err == nameRule, "proto: empty name is rejected");
  check(!parses((head + "a\\nb" + tailSlots).c_str(), r, err) && err == nameRule, "proto: control character in the name");
  check(!parses((head + "caf\\u00e9" + tailSlots).c_str(), r, err) && err == nameRule, "proto: non-ASCII name is rejected");
  check(!parses((head + "caf\xC3\xA9" + tailSlots).c_str(), r, err) && err == nameRule, "proto: raw UTF-8 name is rejected");

  Layout l;
  char why[protocol::kErrorBytes];
  auto resolve = [&](const char* ids) {
    std::string line = std::string("{\"id\":1,\"cmd\":\"set_layout\",\"name\":\"n\",\"slots\":[") + ids + "]}";
    protocol::Request q;
    char e[protocol::kErrorBytes];
    check(protocol::parseRequest(line.c_str(), q, e, sizeof e), "proto: layout line parses");
    why[0] = '\0';
    return protocol::resolveLayout(q, l, why, sizeof why);
  };
  const char* tail = "\"gate\",\"slapback\",\"unison\",\"octave\",\"autotune\",\"distortion\"";
  check(!resolve((std::string("\"input_gate\",\"foo\",") + tail).c_str()) &&
            std::strcmp(why, "unknown card \"foo\" in slot 2") == 0, "proto: unknown card names the id and slot");
  check(!resolve((std::string("\"reverb_spring\",\"reverb_chasm\",") + tail).c_str()) &&
            std::strcmp(why, "two reverb cards") == 0, "proto: two reverbs");
  check(!resolve((std::string("\"slapback\",\"input_gate\",") + tail).c_str()) &&
            std::strcmp(why, "same card twice: slapback") == 0, "proto: duplicate card");
  check(resolve("\"empty\",\"empty\",\"empty\",\"empty\",\"empty\",\"empty\",\"empty\",\"empty\"") && l.card[3] == Card::Empty,
        "proto: all-empty layout is accepted");
}

void checkProtocolReplies() {
  StringWriter w;
  protocol::replyError(w, 12, "unknown card \"foo\" in slot 3\\");
  check(JsonReader(w.out).valid() && has(w.out, "\"id\":12,\"ok\":false,\"error\":\"unknown card \\\"foo\\\" in slot 3\\\\\""),
        "proto: error reply escapes and parses");
  w = StringWriter();
  protocol::replyOk(w, 13);
  check(w.out == "{\"id\":13,\"ok\":true}\n", "proto: ok reply");

  w = StringWriter();
  protocol::replyLayout(w, 14, "Rig \"A\"", defaultLayout());
  check(JsonReader(w.out).valid() && has(w.out, "\"layout\":{\"name\":\"Rig \\\"A\\\"\",\"slots\":[\"input_gate\",\"autotune\"") &&
            has(w.out, "\"reverb_spring\"]}"), "proto: layout reply");

  w = StringWriter();
  protocol::Status s{54.66f, 71.0f, 0, true, 9, 2, 5};
  protocol::replyStatus(w, 15, s);
  check(JsonReader(w.out).valid() &&
            w.out == "{\"id\":15,\"ok\":true,\"cpu_mean\":54.66,\"cpu_worst\":71,\"late\":0,\"muted\":true,\"seq\":9,\"rec_errors\":2,\"serial_dropped\":5}\n",
        "proto: status reply");

  w = StringWriter();
  protocol::replyHello(w, 16, "v2.00.04", "Default", defaultLayout());
  check(JsonReader(w.out).valid(), "proto: hello parses as JSON");
  check(w.calls > 20, "proto: hello streams in pieces");
  check(has(w.out, "\"proto\":1,\"fw\":\"v2.00.04\""), "proto: hello carries proto and fw");
  for (int i = 0; i < kCardCount; ++i) {
    const CardDesc& d = cardDesc(card(i));
    check(has(w.out, std::string("{\"id\":\"") + d.id + "\",\"name\":\"" + d.name + "\""), d.id);
    for (int k = 0; k < d.knobCount; ++k) check(has(w.out, std::string("{\"name\":\"") + d.knob[k].name + "\""), d.knob[k].name);
  }
  check(has(w.out, "\"cost\":{\"avg\":14,\"worst\":21,\"measured\":true}"), "proto: hello carries costs");
  check(has(w.out, "\"kind\":\"stepped\",\"positions\":25,\"min\":-12,\"max\":12,\"unit\":\"st\",\"curve\":\"linear\""),
        "proto: hello carries the octave knob");
  check(has(w.out, "\"curve\":\"log\"") && has(w.out, "\"curve\":\"log_reversed\""), "proto: hello carries curves");
  check(has(w.out, "\"group\":\"pitch\"") && has(w.out, "\"group\":\"reverb\""), "proto: hello carries groups");
  check(has(w.out, "\"layout\":{\"name\":\"Default\""), "proto: hello carries the layout");
}

void checkFramer() {
  protocol::Framer f;
  using K = protocol::Framer::Kind;
  check(f.feed('p') == K::Bench && f.bench() == 'p', "framer: a bare byte is a bench letter");
  check(f.feed('\n') == K::Bench, "framer: a newline outside a line is not a line");
  const char* line = "{\"id\":1}";
  K k = K::None;
  for (const char* c = line; *c; ++c) {
    k = f.feed(static_cast<uint8_t>(*c));
    check(k == K::None, "framer: line bytes are held");
  }
  check(f.feed('\n') == K::Line && std::strcmp(f.line(), line) == 0, "framer: newline ends the line");
  check(f.feed('q') == K::Bench, "framer: bench letters work after a line");

  // letters inside a line belong to the line
  f.feed('{');
  check(f.feed('p') == K::None && f.feed('\n') == K::Line && std::strcmp(f.line(), "{p") == 0, "framer: letters inside a line stay in it");

  // 768 bytes pass, 769 do not
  f.feed('{');
  for (size_t i = 1; i < protocol::kMaxLine; ++i) f.feed('a');
  check(f.feed('\n') == K::Line && std::strlen(f.line()) == protocol::kMaxLine, "framer: a 768-byte line passes");
  f.feed('{');
  for (size_t i = 1; i < protocol::kMaxLine; ++i) f.feed('a');
  check(f.feed('a') == K::TooLong, "framer: byte 769 is too long");
  check(f.feed('b') == K::None && f.feed('{') == K::None && f.feed('\n') == K::None, "framer: the rest of a long line is dropped");
  check(f.feed('m') == K::Bench && f.bench() == 'm', "framer: bench letters resume after a dropped line");

  // a host that left mid-line does not leak into the next session
  for (const char* c = "{\"id\":1,\"cm"; *c; ++c) f.feed(static_cast<uint8_t>(*c));
  f.reset();
  check(f.feed('d') == K::Bench, "framer: a new session starts outside a line");
  for (const char* c = line; *c; ++c) f.feed(static_cast<uint8_t>(*c));
  check(f.feed('\n') == K::Line && std::strcmp(f.line(), line) == 0, "framer: the first line after a reset is whole");
  f.feed('{');
  for (size_t i = 1; i <= protocol::kMaxLine; ++i) f.feed('a');
  f.reset();
  check(f.feed('q') == K::Bench, "framer: a reset ends a dropped line");
}

void checkLayoutFade() {
  LayoutFade fade;
  LayoutFade::Block b = fade.next(false);
  check(b.from == 1.0f && b.to == 1.0f && !b.swap, "fade: idle passes at unity");

  // Run a whole fade; the new layout is pending until the swap.
  float last = 1.0f;
  int outBlocks = 0;
  int inBlocks = 0;
  int swaps = 0;
  bool swapped = false;
  bool monotone = true;
  bool continuous = true;
  for (int i = 0; i < 4 * kFadeBlocks; ++i) {
    b = fade.next(!swapped);
    if (b.from != last) continuous = false;
    if (b.swap) {
      ++swaps;
      swapped = true;
      check(b.from == 0.0f && last == 0.0f, "fade: the swap block starts at zero");
    }
    if (!swapped) {
      ++outBlocks;
      if (b.to > b.from) monotone = false;
    } else {
      if (b.to > b.from) ++inBlocks;  // the silent hold blocks are not part of the ramp
      if (b.to < b.from) monotone = false;
    }
    last = b.to;
    if (swapped && b.to == 1.0f) break;
  }
  check(swaps == 1, "fade: one swap per layout change");
  check(monotone && continuous, "fade: gain falls then rises with no jump");
  check(outBlocks == kFadeBlocks && inBlocks == kFadeBlocks, "fade: each ramp takes kFadeBlocks blocks");
  check(last == 1.0f, "fade: gain returns to unity");
  check(kFadeBlocks * cv::kBlock * 1000 / cv::kSampleRate >= kFadeMs && kFadeBlocks * cv::kBlock * 1000 / cv::kSampleRate <= kFadeMs + 2,
        "fade: each ramp lasts about 50 ms");
  b = fade.next(false);
  check(b.from == 1.0f && b.to == 1.0f && !b.swap, "fade: idle again after the ramp");

  // A second layout during the fade-out is taken at the same zero point: one swap, one fade.
  fade = LayoutFade();
  swaps = 0;
  for (int i = 0; i < 4 * kFadeBlocks; ++i) {
    b = fade.next(true);
    if (b.swap) {
      ++swaps;
      break;
    }
  }
  check(swaps == 1, "fade: a second layout mid-fade waits for the same zero point");

  // A layout arriving during the fade-in turns the ramp down again from where it is.
  fade = LayoutFade();
  bool pending = true;
  float at = 0.0f;
  for (int i = 0; i < 2 * kFadeBlocks; ++i) {
    b = fade.next(pending);
    if (b.swap) pending = false;
    if (b.swap) {
      for (int j = 0; j < 10; ++j) at = fade.next(false).to;
      break;
    }
  }
  b = fade.next(true);
  check(b.from == at && b.to < b.from && !b.swap, "fade: a new layout during fade-in ramps down from the current gain");

  // The layout changing back during the fade-out turns the ramp up with no swap.
  fade = LayoutFade();
  for (int i = 0; i < 10; ++i) b = fade.next(true);
  const float turn = b.to;
  b = fade.next(false);
  check(b.from == turn && b.to > b.from && !b.swap, "fade: changing back mid-fade-out ramps up without a swap");
}

void checkEnginesToReset() {
  const Layout base = defaultLayout();
  auto bit = [](Engine e) { return 1u << static_cast<int>(e); };
  check(enginesToReset(base, base) == 0, "reset: same layout resets nothing");

  Layout swapped = base;
  swapped.card[1] = Card::Octave;
  swapped.card[2] = Card::Autotune;
  check(enginesToReset(base, swapped) == bit(Engine::Pitch), "reset: swapping the two pitch cards resets the pitch engine once");

  Layout chasm = base;
  chasm.card[7] = Card::ReverbChasm;
  check(enginesToReset(base, chasm) == bit(Engine::Reverb), "reset: Spring to Chasm resets the reverb engine");

  Layout moved = base;
  moved.card[4] = Card::Empty;
  moved.card[0] = Card::Slapback;
  moved.card[1] = Card::Autotune;
  check(enginesToReset(base, moved) == (bit(Engine::InputGate) | bit(Engine::Slapback)),
        "reset: only engines in changed slots reset, Autotune stayed put");

  Layout empty = base;
  empty.card[2] = Card::Empty;
  check(enginesToReset(base, empty) == bit(Engine::Pitch), "reset: a card leaving a slot resets its engine");
  check(enginesToReset(empty, empty) == 0 && enginesToReset(Layout{{}}, Layout{{}}) == 0, "reset: empty slots reset nothing");
}

void checkReplyEscapes() {
  StringWriter w;
  protocol::replyLayout(w, 1, "caf\xC3\xA9\x01\x7F", defaultLayout());
  check(JsonReader(w.out).valid() && has(w.out, "\"name\":\"caf\\u00c3\\u00a9\\u0001\\u007f\""),
        "proto: non-ASCII and control bytes in a reply string are escaped");
}

void checkFadeHold() {
  LayoutFade fade(3);
  LayoutFade::Block b;
  int swaps = 0;
  int zeroBlocks = 0;
  bool inFade = false;
  float last = 1.0f;
  for (int i = 0; i < 6 * kFadeBlocks; ++i) {
    b = fade.next(swaps == 0);
    if (b.swap) ++swaps;
    if (swaps > 0 && b.from == 0.0f && b.to == 0.0f) ++zeroBlocks;
    if (swaps > 0 && b.to > b.from) inFade = true;
    check(b.from == last, "fade hold: no jump in gain");
    last = b.to;
    if (inFade && b.to == 1.0f) break;
  }
  check(swaps == 1 && zeroBlocks == 4 && last == 1.0f, "fade hold: the swap block plus 3 held blocks stay at zero, then the ramp returns");
}

void checkTxQueue() {
  TxQueue q;
  const char* run;
  q.put("abc", 3);
  check(q.pending() == 0 && q.peek(run) == 0, "tx: an unfinished line is held back");
  q.put("d\nef\n", 5);
  check(q.pending() == 8 && q.peek(run) == 8 && std::memcmp(run, "abcd\nef\n", 8) == 0, "tx: finished lines are ready");
  q.consume(5);
  check(q.pending() == 3, "tx: consume advances");
  q.consume(3);

  // A line that does not fit is dropped whole, counted once, and the queue keeps working.
  const std::string big(TxQueue::kCapacity - 4, 'x');
  q.put("keep\n", 5);
  q.put(big.data(), big.size());
  q.put("\nnext\n", 6);
  check(q.dropped() == 1, "tx: an overflowing line is counted");
  size_t got = 0;
  std::string seen;
  while ((got = q.peek(run)) > 0) {
    seen.append(run, got);
    q.consume(got);
  }
  check(seen == "keep\nnext\n", "tx: only whole lines survive an overflow");

  // Wrap-around: the ring hands out contiguous runs.
  for (int round = 0; round < 3; ++round) {
    const std::string line(TxQueue::kCapacity / 2 + 100, 'a' + round);
    q.put(line.data(), line.size());
    q.put("\n", 1);
    seen.clear();
    while ((got = q.peek(run)) > 0) {
      seen.append(run, got);
      q.consume(got);
    }
    check(seen == line + "\n", "tx: lines survive the ring wrapping");
  }

  q.put("lost\n", 5);
  q.discard();
  check(q.pending() == 0 && q.dropped() == 1, "tx: discard empties the queue without counting");
}

// Deterministic generator so a failure reproduces.
struct Lcg {
  uint32_t s;
  uint32_t next() {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
  }
};

void parseAndResolve(const char* line) {
  protocol::Request req;
  char err[protocol::kErrorBytes];
  if (protocol::parseRequest(line, req, err, sizeof err) && req.cmd == protocol::Command::SetLayout) {
    Layout l = defaultLayout();
    protocol::resolveLayout(req, l, err, sizeof err);
  }
}

void checkFuzz() {
  const std::string valid =
      "{\"id\":3,\"cmd\":\"set_layout\",\"name\":\"Stage\",\"slots\":[\"input_gate\",\"autotune\",\"octave\","
      "\"unison\",\"slapback\",\"distortion\",\"gate\",\"reverb_chasm\"],\"extra\":{\"a\":[1,2,{\"b\":null}]}}";
  Lcg rng{12345};

  // every prefix of a valid line, and the line with each single byte replaced
  for (size_t n = 0; n <= valid.size(); ++n) parseAndResolve(valid.substr(0, n).c_str());
  for (size_t i = 0; i < valid.size(); ++i) {
    std::string s = valid;
    for (int k = 0; k < 8; ++k) {
      s[i] = static_cast<char>(rng.next());
      if (s[i] == '\0') s[i] = '?';
      parseAndResolve(s.c_str());
    }
  }

  // random JSON-flavored soup and raw bytes
  const char soup[] = "{}[]\",:\\ \t0123456789-+.eEtrufalsn\"idcmdnameslots";
  for (int i = 0; i < 20000; ++i) {
    std::string s;
    const size_t len = rng.next() % 200;
    for (size_t k = 0; k < len; ++k) {
      const bool raw = i % 4 == 0;
      char c = raw ? static_cast<char>(rng.next()) : soup[rng.next() % (sizeof soup - 1)];
      if (c == '\0') c = ' ';
      s.push_back(c);
    }
    parseAndResolve(s.c_str());
  }

  // deep nesting and long strings must not blow the stack or the buffers
  parseAndResolve(("{\"x\":" + std::string(5000, '[') + std::string(5000, ']') + "}").c_str());
  parseAndResolve(("{\"x\":" + std::string(5000, '{') + "}").c_str());
  parseAndResolve(("{\"id\":1,\"cmd\":\"" + std::string(5000, 'x') + "\"}").c_str());

  // the framer sees every byte value in any order
  protocol::Framer f;
  size_t lines = 0;
  for (int i = 0; i < 200000; ++i) {
    const uint8_t byte = (i % 7 == 0) ? '\n' : (i % 11 == 0 ? '{' : static_cast<uint8_t>(rng.next()));
    const protocol::Framer::Kind k = f.feed(byte);
    if (k == protocol::Framer::Kind::Line) {
      ++lines;
      check(std::strlen(f.line()) <= protocol::kMaxLine, "fuzz: framed line fits the limit");
      parseAndResolve(f.line());
    }
  }
  check(lines > 100, "fuzz: the framer produced lines");
}

int main() {
  checkIds();
  checkGroups();
  checkKnobsMove();
  checkKnobTargets();
  checkDescriptors();
  checkToggles();
  checkPlacement();
  checkFormat();
  checkDefaultLayout();
  checkSettings();
  checkLayoutValid();
  checkProtocolCommands();
  checkProtocolErrors();
  checkProtocolReplies();
  checkFramer();
  checkLayoutFade();
  checkFadeHold();
  checkReplyEscapes();
  checkTxQueue();
  checkFuzz();
  checkEnginesToReset();
  checkComposeToggles();
  checkComposeBench();
  checkOutputGain();
  checkDebounce();
  checkSteppedPosition();
  checkKnobFilter();
  checkMenu();
  checkMenuTimeout();
  checkMenuRedraw();
  checkMenuInvalidate();
  checkCanvas();
  checkScreenshots();
  checkJackSwitch();
  checkVersionTag();
  checkMuteSequence();
  checkMuteLatchAndHold();
  checkPanelTest();
  checkRecords();
  std::printf("firmware_test: version %s, %d failures\n", kFirmwareVersion, gFailures);
  return gFailures == 0 ? 0 : 1;
}
