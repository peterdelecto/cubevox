#include "board/bench.h"

#include <Arduino.h>

#include "board/audio.h"
#include "board/controls.h"
#include "board/mute.h"
#include "board/platform.h"
#include "board/record_service.h"
#include "board/slots.h"
#include "core/panel_test.h"

namespace {

BenchOverrides gBench;
bool gClipOn = false;

PanelTest gPanel;
bool gPanelOn = false;
bool gOledPattern = false;
bool gOledDrawn = false;
bool gMuteHeld = false;
bool gJackForced = false;
InputJack gJackForce = InputJack::Mic;
InputJack gJackEffective = InputJack::Mic;

void printSlotRow(const char* label, const uint32_t* values) {
  Serial.print(label);
  for (int i = 0; i <= kSlotCount; ++i) {
    Serial.print(' ');
    Serial.print(values[i]);
  }
}

void forceBusFault() {
  Serial.println("[bench] forcing a bus fault");
  Serial.flush();
  (void)*reinterpret_cast<volatile uint32_t*>(0xDEADBEE0);
}

[[noreturn]] void spinForWatchdog() {
  Serial.println("[bench] spinning; the watchdog resets in 2 s");
  Serial.flush();
  for (;;) {}
}

void enterDfu() {
  Serial.println("[bench] entering DFU");
  Serial.flush();
  muteForReset();
  platformEnterDfu();
}

void toggleKnobsPinned() {
  gBench.knobsPinned = !gBench.knobsPinned;
  Serial.println(gBench.knobsPinned ? "[bench] knobs pinned at top" : "[bench] knobs live");
}

void toggleReverbChasm() {
  gBench.reverbChasm = !gBench.reverbChasm;
  Serial.println(gBench.reverbChasm ? "[bench] reverb slot runs Chasm"
                                    : "[bench] reverb slot runs its card's engine");
}

void toggleClip() {
  gClipOn = audioBenchClip(!gClipOn);
  Serial.println(gClipOn ? "[bench] vocal clip on" : "[bench] vocal clip off (or not compiled in)");
}

void resetPeaks() {
  audioResetPeaks();
  Serial.println("[bench] audio peaks reset");
}

void stopAudio() {
  Serial.println("[bench] stopping audio DMA");
  audioStopForBench();
}

void printTwoDigits(int n) {
  if (n < 10) Serial.print('0');
  Serial.print(n);
}

// "pot NN slot S A|B", slots numbered 1..8 as on the panel.
void printPotId(int channel) {
  const PotOwner owner = potOwner(channel);
  Serial.print("pot ");
  printTwoDigits(potNumber(channel));
  Serial.print(" slot ");
  Serial.print(owner.slot + 1);
  Serial.print(owner.knobB ? " B " : " A ");
}

void printToggle(int slot, bool on) {
  Serial.print("toggle slot ");
  Serial.print(slot + 1);
  Serial.println(on ? " on" : " off");
}

void panelSummary() {
  Serial.print("[panel] pots ");
  Serial.print(gPanel.potsDone());
  Serial.print("/16 todo");
  for (int i = 0; i < kPanelTestPots; ++i) {
    if (gPanel.potDone(i)) continue;
    Serial.print(' ');
    printTwoDigits(potNumber(i));
  }
  Serial.print("; toggles ");
  Serial.print(gPanel.togglesDone());
  Serial.print("/8 todo");
  for (int i = 0; i < kPanelTestToggles; ++i) {
    if (gPanel.toggleDone(i)) continue;
    Serial.print(' ');
    Serial.print(i + 1);
  }
  Serial.println();
}

void panelDump() {
  for (int ch = 0; ch < kPotCount; ++ch) {
    Serial.print("[panel] raw ");
    printPotId(ch);
    Serial.println(controlsRawPot(ch), 2);
  }
  for (int s = 0; s < kSlotCount; ++s) {
    Serial.print("[panel] raw ");
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
    Serial.println("[panel] test on (g dump, l summary, i K103, x mute, o OLED, t off)");
    return;
  }
  panelEnd();
  panelSummary();
  Serial.println("[panel] test off");
}

// The commands below only act while the test is on, so leaving it restores the box.
bool requirePanelTest() {
  if (!gPanelOn) Serial.println("[panel] start the test first with t");
  return gPanelOn;
}

void stepRelay() {
  if (!requirePanelTest()) return;
  gJackForced = true;
  gJackForce = gJackEffective == InputJack::Mic ? InputJack::Pedal : InputJack::Mic;
  Serial.println(gJackForce == InputJack::Pedal ? "[panel] K103 to PEDAL (PAD_EN high)"
                                                : "[panel] K103 to MIC (PAD_EN low)");
}

void toggleMute() {
  if (!requirePanelTest()) return;
  gMuteHeld = !gMuteHeld;
  Serial.println(gMuteHeld ? "[panel] mute held (XSMT low, then K102 opens)"
                           : "[panel] mute released (K102 closes, then XSMT high)");
}

void showOledPattern() {
  if (!requirePanelTest()) return;
  gOledPattern = true;
  gOledDrawn = false;
  Serial.println("[panel] OLED pattern up until t");
}

// Runs one command. Returns true when it changed the overrides.
bool run(int c, void (*bootReport)()) {
  if (c >= '1' && c <= '8') {
    gBench.slotsOff ^= static_cast<uint8_t>(1u << (c - '1'));
    benchPrintStats();
    return true;
  }
  switch (c) {
    case '0':
      gBench.slotsOff = 0;
      benchPrintStats();
      return true;
    case 'k': toggleKnobsPinned(); return true;
    case 'c': toggleReverbChasm(); return true;
    case 'r': bootReport(); break;
    case 'p': benchPrintStats(); break;
    case 'f': forceBusFault(); break;
    case 'w': spinForWatchdog();
    case 'a': stopAudio(); break;
    case 'm': resetPeaks(); break;
    case 'v': toggleClip(); break;
    case 'd': enterDfu(); break;
    case 's': recordSaveNow(); break;
    case 'q': recordPrintStatus(Serial); break;
    case 'e': recordEraseAll(); break;
    case 't': togglePanelTest(); break;
    case 'l': if (requirePanelTest()) panelSummary(); break;
    case 'g': panelDump(); break;
    case 'i': stepRelay(); break;
    case 'x': toggleMute(); break;
    case 'o': showOledPattern(); break;
    default: break;
  }
  return false;
}

}  // namespace

const BenchOverrides& benchOverrides() { return gBench; }

void benchPrintStats() {
  const AudioStats s = audioStats();
  Serial.print("[audio] blocks ");
  Serial.print(s.blocks);
  Serial.print(" late ");
  Serial.print(s.late);
  Serial.print(" overruns ");
  Serial.print(s.overruns);
  Serial.print(" avg ");
  Serial.print(s.avgCycles);
  Serial.print(" worst ");
  Serial.print(s.maxCycles);
  Serial.print(" of ");
  Serial.print(s.budgetCycles);
  Serial.print(" cycles, slots off 0x");
  Serial.println(gBench.slotsOff, HEX);
  printSlotRow("[audio] per slot", s.slotAvg);
  Serial.println(" (last is EQ)");
  printSlotRow("[audio] slot worst", s.slotWorst);
  Serial.println();
}

bool benchPoll(void (*bootReport)()) {
  bool changed = false;
  while (Serial.available() > 0) {
    if (run(Serial.read(), bootReport)) changed = true;
  }
  return changed;
}

void benchPanelPoll(uint32_t nowMs) {
  if (!gPanelOn || !controlsSeeded()) return;
  for (int ch = 0; ch < kPotCount; ++ch) {
    const float v = controlsRawPot(ch);
    if (!gPanel.updatePot(ch, v, nowMs)) continue;
    Serial.print("[panel] ");
    printPotId(ch);
    Serial.println(v, 2);
  }
  for (int s = 0; s < kSlotCount; ++s) {
    const bool on = controlsToggleOn(s);
    if (gPanel.updateToggle(s, on)) {
      Serial.print("[panel] ");
      printToggle(s, on);
    }
  }
}

InputJack benchJack(InputJack requested) {
  gJackEffective = gJackForced ? gJackForce : requested;
  return gJackEffective;
}

bool benchMuteHeld() { return gMuteHeld; }

bool benchDrawPanelPattern(Canvas& canvas) {
  if (!gOledPattern) return false;
  if (!gOledDrawn) {
    drawPanelTestPattern(canvas);
    gOledDrawn = true;
  }
  return true;
}
