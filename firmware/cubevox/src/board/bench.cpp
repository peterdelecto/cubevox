#include "board/bench.h"

#include <Arduino.h>

#include "board/audio.h"
#include "board/controls.h"
#include "board/mute.h"
#include "board/platform.h"
#include "board/record_service.h"
#include "board/serial_link.h"
#include "board/slots.h"
#include "core/panel_test.h"

namespace {

constexpr uint32_t kFlushMs = 100;  // queued text gets this long to leave before a fault, spin or DFU

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
  serialOut().print(label);
  for (int i = 0; i <= kSlotCount; ++i) {
    serialOut().print(' ');
    serialOut().print(values[i]);
  }
}

void forceBusFault() {
  serialOut().println("[bench] forcing a bus fault");
  serialFlush(kFlushMs);
  (void)*reinterpret_cast<volatile uint32_t*>(0xDEADBEE0);
}

[[noreturn]] void spinForWatchdog() {
  serialOut().println("[bench] spinning; the watchdog resets in 2 s");
  serialFlush(kFlushMs);
  for (;;) {}
}

void enterDfu() {
  serialOut().println("[bench] entering DFU");
  serialFlush(kFlushMs);
  muteForReset();
  platformEnterDfu();
}

void toggleKnobsPinned() {
  gBench.knobsPinned = !gBench.knobsPinned;
  serialOut().println(gBench.knobsPinned ? "[bench] knobs pinned at top" : "[bench] knobs live");
}

void toggleReverbChasm() {
  gBench.reverbChasm = !gBench.reverbChasm;
  serialOut().println(gBench.reverbChasm ? "[bench] reverb slot runs Chasm"
                                    : "[bench] reverb slot runs its card's engine");
}

void toggleClip() {
  gClipOn = audioBenchClip(!gClipOn);
  serialOut().println(gClipOn ? "[bench] vocal clip on" : "[bench] vocal clip off (or not compiled in)");
}

void resetPeaks() {
  audioResetPeaks();
  serialOut().println("[bench] audio peaks reset");
}

void stopAudio() {
  serialOut().println("[bench] stopping audio DMA");
  audioStopForBench();
}

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
  serialOut().print("/16 todo");
  for (int i = 0; i < kPanelTestPots; ++i) {
    if (gPanel.potDone(i)) continue;
    serialOut().print(' ');
    printTwoDigits(potNumber(i));
  }
  serialOut().print("; toggles ");
  serialOut().print(gPanel.togglesDone());
  serialOut().print("/8 todo");
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
    serialOut().println("[panel] test on (g dump, l summary, i K103, x mute, o OLED, t off)");
    return;
  }
  panelEnd();
  panelSummary();
  serialOut().println("[panel] test off");
}

// The commands below only act while the test is on, so leaving it restores the box.
bool requirePanelTest() {
  if (!gPanelOn) serialOut().println("[panel] start the test first with t");
  return gPanelOn;
}

void stepRelay() {
  if (!requirePanelTest()) return;
  gJackForced = true;
  gJackForce = gJackEffective == InputJack::Mic ? InputJack::Pedal : InputJack::Mic;
  serialOut().println(gJackForce == InputJack::Pedal ? "[panel] K103 to PEDAL (PAD_EN high)"
                                                : "[panel] K103 to MIC (PAD_EN low)");
}

void toggleMute() {
  if (!requirePanelTest()) return;
  gMuteHeld = !gMuteHeld;
  serialOut().println(gMuteHeld ? "[panel] mute held (XSMT low, then K102 opens)"
                           : "[panel] mute released (K102 closes, then XSMT high)");
}

void showOledPattern() {
  if (!requirePanelTest()) return;
  gOledPattern = true;
  gOledDrawn = false;
  serialOut().println("[panel] OLED pattern up until t");
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
    case 'q': recordPrintStatus(serialOut()); break;
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
  serialOut().print("[audio] blocks ");
  serialOut().print(s.blocks);
  serialOut().print(" late ");
  serialOut().print(s.late);
  serialOut().print(" overruns ");
  serialOut().print(s.overruns);
  serialOut().print(" avg ");
  serialOut().print(s.avgCycles);
  serialOut().print(" worst ");
  serialOut().print(s.maxCycles);
  serialOut().print(" of ");
  serialOut().print(s.budgetCycles);
  serialOut().print(" cycles, slots off 0x");
  serialOut().println(gBench.slotsOff, HEX);
  printSlotRow("[audio] per slot", s.slotAvg);
  serialOut().println(" (last is EQ)");
  printSlotRow("[audio] slot worst", s.slotWorst);
  serialOut().println();
  const AudioSwapStats sw = audioSwapStats();
  serialOut().print("[audio] layout swaps ");
  serialOut().print(sw.count);
  serialOut().print(", last: swap block ");
  serialOut().print(sw.swapBlockCycles);
  serialOut().print(", worst block through the fade-in ");
  serialOut().print(sw.worstCycles);
  serialOut().print(" of ");
  serialOut().print(s.budgetCycles);
  serialOut().println(" cycles");
}

bool benchCommand(char c, void (*bootReport)()) { return run(c, bootReport); }

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

bool benchDrawPanelPattern(Canvas& canvas) {
  if (!gOledPattern) return false;
  if (!gOledDrawn) {
    drawPanelTestPattern(canvas);
    gOledDrawn = true;
  }
  return true;
}
