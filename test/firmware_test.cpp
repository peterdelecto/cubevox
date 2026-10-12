// Firmware core tier on the host: registry apply, descriptors, default layout, costs.
// Compiles firmware/cubevox/src/core/*.cpp; a core file that pulls in Arduino breaks this build.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/canvas.h"
#include "core/chain_params.h"
#include "core/compose.h"
#include "core/costs.h"
#include "core/display.h"
#include "core/fonts.h"
#include "core/jack_switch.h"
#include "core/knob_filter.h"
#include "core/layout.h"
#include "core/panel.h"
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
  checkComposeToggles();
  checkComposeBench();
  checkOutputGain();
  checkDebounce();
  checkSteppedPosition();
  checkKnobFilter();
  checkMenu();
  checkMenuTimeout();
  checkMenuRedraw();
  checkCanvas();
  checkScreenshots();
  checkJackSwitch();
  std::printf("firmware_test: version %s, %d failures\n", kFirmwareVersion, gFailures);
  return gFailures == 0 ? 0 : 1;
}
