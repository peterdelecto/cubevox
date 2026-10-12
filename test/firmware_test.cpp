// Firmware core tier on the host: registry apply, descriptors, default layout, costs.
// Compiles firmware/cubevox/src/core/*.cpp; a core file that pulls in Arduino breaks this build.

#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/chain_params.h"
#include "core/compose.h"
#include "core/costs.h"
#include "core/display.h"
#include "core/jack_switch.h"
#include "core/knob_filter.h"
#include "core/layout.h"
#include "core/panel.h"
#include "core/registry.h"
#include "core/settings.h"
#include "core/version.h"
#include "ui/menu.h"

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
    check(d.measured, "every card's cost comes from the board");
    check(d.cost.worst >= d.cost.avg, "worst block is never under the mean");
  }
}

void checkLayoutValid() {
  check(layoutValid(defaultLayout()), "default layout is valid");
  Layout bad = defaultLayout();
  bad.card[3] = static_cast<Card>(kCardCount);
  check(!layoutValid(bad), "out-of-range card id is rejected");
  Layout twoReverbs = defaultLayout();
  twoReverbs.card[0] = Card::ReverbChasm;
  check(!layoutValid(twoReverbs), "second reverb card is rejected");
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

// Records the rows the menu draws.
class RecordingDisplay : public Display {
 public:
  void clear() override { std::memset(rows_, 0, sizeof(rows_)); inverted_ = -1; }
  void text(int r, const char* line) override { std::snprintf(rows_[r], sizeof(rows_[r]), "%s", line); }
  void invertRow(int r) override { inverted_ = r; }
  void flush() override { ++flushes_; }
  const char* rowText(int r) const { return rows_[r]; }
  int inverted() const { return inverted_; }
  int flushes() const { return flushes_; }

 private:
  char rows_[kDisplayRows][kDisplayCols + 1] = {};
  int inverted_ = -1;
  int flushes_ = 0;
};

struct MenuRig {
  Menu menu;
  Layout layout = defaultLayout();
  PanelReading panel;
  Settings settings;
  RecordingDisplay display;
  uint32_t nowMs = 100;

  UiAction step(int detents, bool click, uint32_t advanceMs = 10) {
    nowMs += advanceMs;
    UiInput in;
    in.detents = detents;
    in.click = click;
    return menu.update(in, nowMs);
  }
  void draw() { menu.draw(UiContext{layout, panel, settings, false}, display); }
};

void checkMenu() {
  MenuRig m;
  m.draw();
  check(std::strcmp(m.display.rowText(0), "cubevox") == 0, "menu: idle title");

  m.step(0, true);
  m.draw();
  check(std::strcmp(m.display.rowText(0), "MENU") == 0, "menu: click opens the list");
  check(m.display.inverted() == 1 && std::strcmp(m.display.rowText(1), ">Out LINE") == 0, "menu: cursor 0 row inverted");
  check(std::strcmp(m.display.rowText(2), " Input MIC") == 0, "menu: input row text");

  m.step(-1, false);
  m.draw();
  check(m.display.inverted() == 5, "menu: turning back from 0 wraps to the last item");
  m.step(1, false);
  m.draw();
  check(m.display.inverted() == 1, "menu: turning forward wraps to 0");

  check(m.step(0, true) == UiAction::ToggleOutput, "menu: Out click toggles output");
  m.step(1, false);
  check(m.step(0, true) == UiAction::ToggleInputJack, "menu: Input click toggles the jack");
  m.settings.inputJack = InputJack::Pedal;
  m.draw();
  check(std::strcmp(m.display.rowText(2), ">Input PEDAL") == 0, "menu: row reads the settings");

  m.step(1, false);
  check(m.step(0, true) == UiAction::None, "menu: Chain order click has no action");
  m.draw();
  check(std::strncmp(m.display.rowText(0), "1 ", 2) == 0, "menu: chain view starts at slot 1");
  check(std::strncmp(m.display.rowText(kDisplayRows - 1), "8 ", 2) == 0, "menu: chain view shows slot 8");
  m.step(0, true);
  m.draw();
  check(std::strcmp(m.display.rowText(0), "MENU") == 0, "menu: click leaves the chain view");

  m.step(1, false);
  m.step(0, true);
  m.draw();
  check(std::strcmp(m.display.rowText(0), "UPDATE FIRMWARE") == 0, "menu: confirm screen");
  m.step(1, false);
  m.draw();
  check(std::strcmp(m.display.rowText(0), "MENU") == 0, "menu: turn cancels the confirm");
  m.step(0, true);
  m.step(0, true);
  check(m.step(0, true) == UiAction::EnterDfu, "menu: click on confirm enters DFU");
}

void checkMenuTimeout() {
  MenuRig m;
  m.step(0, true);
  m.step(0, false, 14000);
  m.draw();
  check(std::strcmp(m.display.rowText(0), "MENU") == 0, "menu: still open before the timeout");
  m.step(0, false, 1500);
  m.draw();
  check(std::strcmp(m.display.rowText(0), "cubevox") == 0, "menu: 15 s without input returns to idle");
}

void checkMenuRedraw() {
  MenuRig m;
  m.draw();
  m.draw();
  check(m.display.flushes() <= 1, "menu: an unchanged frame is not redrawn");
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

int main() {
  checkIds();
  checkGroups();
  checkKnobsMove();
  checkKnobTargets();
  checkToggles();
  checkPlacement();
  checkFormat();
  checkDefaultLayout();
  checkSettings();
  checkLayoutValid();
  checkComposeToggles();
  checkComposeBench();
  checkOutputGain();
  checkDebounce();
  checkSteppedPosition();
  checkKnobFilter();
  checkMenu();
  checkMenuTimeout();
  checkMenuRedraw();
  checkJackSwitch();
  std::printf("firmware_test: version %s, %d failures\n", kFirmwareVersion, gFailures);
  return gFailures == 0 ? 0 : 1;
}
