#include "board/bench_panel.h"

#if CUBEVOX_BENCH

#include <Arduino.h>

#include "board/controls.h"
#include "board/serial_link.h"
#include "board/slots.h"
#include "core/loop_policy.h"
#include "core/panel_test.h"

namespace {

PanelTest gPanel;
bool gPanelOn = false;
bool gOledPattern = false;
bool gOledDrawn = false;
CanvasHandoff gHandoff;
bool gMuteHeld = false;
bool gJackForced = false;
InputJack gJackForce = InputJack::Mic;
InputJack gJackEffective = InputJack::Mic;

void printTwoDigits(int n) {
  if (n < 10) serialOut().print('0');
  serialOut().print(n);
}

// "pot NN slot S A|B", slots numbered 1..8 as on the panel.
void printPotId(int channel) {
  const PotOwner owner = potOwner(channel);
  serialOut().print("pot ");
  printTwoDigits(potNumber(channel));
  serialOut().print(" slot ");
  serialOut().print(owner.slot + 1);
  serialOut().print(owner.knobB ? " B " : " A ");
}

void printToggle(int slot, bool on) {
  serialOut().print("toggle slot ");
  serialOut().print(slot + 1);
  serialOut().println(on ? " on" : " off");
}

void panelSummary() {
  serialOut().print("[panel] pots ");
  serialOut().print(gPanel.potsDone());
  serialOut().print('/');
  serialOut().print(kPanelTestPots);
  serialOut().print(" todo");
  for (int i = 0; i < kPanelTestPots; ++i) {
    if (gPanel.potDone(i)) continue;
    serialOut().print(' ');
    printTwoDigits(potNumber(i));
  }
  serialOut().print("; toggles ");
  serialOut().print(gPanel.togglesDone());
  serialOut().print('/');
  serialOut().print(kPanelTestToggles);
  serialOut().print(" todo");
  for (int i = 0; i < kPanelTestToggles; ++i) {
    if (gPanel.toggleDone(i)) continue;
    serialOut().print(' ');
    serialOut().print(i + 1);
  }
  serialOut().println();
}

void panelDump() {
  for (int ch = 0; ch < kPotCount; ++ch) {
    serialOut().print("[panel] raw ");
    printPotId(ch);
    serialOut().println(controlsRawPot(ch), 2);
  }
  for (int s = 0; s < kSlotCount; ++s) {
    serialOut().print("[panel] raw ");
    printToggle(s, controlsToggleOn(s));
  }
}

void panelEnd() {
  gOledPattern = false;
  gMuteHeld = false;
  gJackForced = false;
}

void togglePanelTest() {
  gPanelOn = !gPanelOn;
  if (gPanelOn) {
    gPanel.reset();
    serialOut().println("[panel] test on (panel_dump, panel_summary, panel_jack, panel_mute, panel_oled, panel_test off)");
    return;
  }
  panelEnd();
  panelSummary();
  serialOut().println("[panel] test off");
}

// The ops below only act while the test is on.
bool requirePanelTest() {
  if (!gPanelOn) serialOut().println("[panel] start the test first with panel_test");
  return gPanelOn;
}

void stepRelay() {
  gJackForced = true;
  gJackForce = gJackEffective == InputJack::Mic ? InputJack::Line : InputJack::Mic;
  serialOut().println(gJackForce == InputJack::Line ? "[panel] K103 to LINE (PAD_EN high)"
                                                : "[panel] K103 to MIC (PAD_EN low)");
}

void toggleMute() {
  gMuteHeld = !gMuteHeld;
  serialOut().println(gMuteHeld ? "[panel] mute held (XSMT low, then K102 opens)"
                           : "[panel] mute released (K102 closes, then XSMT high)");
}

void showOledPattern() {
  gOledPattern = true;
  gOledDrawn = false;
  serialOut().println("[panel] OLED pattern up until panel_test");
}

}  // namespace

const char* benchPanelRun(BenchOp op) {
  if (op == BenchOp::PanelTest) {
    togglePanelTest();
    return nullptr;
  }
  if (op == BenchOp::PanelDump) {
    panelDump();
    return nullptr;
  }
  if (!requirePanelTest()) return "panel test is off";
  switch (op) {
    case BenchOp::PanelSummary: panelSummary(); break;
    case BenchOp::PanelJack: stepRelay(); break;
    case BenchOp::PanelMute: toggleMute(); break;
    case BenchOp::PanelOled: showOledPattern(); break;
    default: return "op not handled";
  }
  return nullptr;
}

void benchPanelPoll(uint32_t nowMs) {
  if (!gPanelOn || !controlsSeeded()) return;
  for (int ch = 0; ch < kPotCount; ++ch) {
    const float v = controlsRawPot(ch);
    if (!gPanel.updatePot(ch, v, nowMs)) continue;
    serialOut().print("[panel] ");
    printPotId(ch);
    serialOut().println(v, 2);
  }
  for (int s = 0; s < kSlotCount; ++s) {
    const bool on = controlsToggleOn(s);
    if (gPanel.updateToggle(s, on)) {
      serialOut().print("[panel] ");
      printToggle(s, on);
    }
  }
}

InputJack benchJack(InputJack requested) {
  gJackEffective = gJackForced ? gJackForce : requested;
  return gJackEffective;
}

bool benchMuteHeld() { return gMuteHeld; }

BenchPattern benchPanelPattern(Canvas& canvas) {
  if (gHandoff.update(gOledPattern)) return BenchPattern::Ended;
  if (!gOledPattern) return BenchPattern::Off;
  if (!gOledDrawn) {
    drawPanelTestPattern(canvas);
    gOledDrawn = true;
  }
  return BenchPattern::Up;
}

#endif
