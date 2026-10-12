#include "board/bench.h"

#include <Arduino.h>

#include "board/audio.h"
#include "board/mute.h"
#include "board/platform.h"

namespace {

BenchOverrides gBench;
bool gClipOn = false;

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
