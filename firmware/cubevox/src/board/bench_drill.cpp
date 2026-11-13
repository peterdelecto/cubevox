#include "board/bench_drill.h"

#if CUBEVOX_BENCH

#include <Arduino.h>

#include "board/audio.h"
#include "board/bench.h"
#include "board/bench_panel.h"
#include "board/fault.h"
#include "board/record_service.h"
#include "board/serial_link.h"

namespace {

BenchOverrides gBench;

constexpr uint8_t kAllSlotsMask = static_cast<uint8_t>((1u << kSlotCount) - 1u);

uint8_t slotBit(int slot) { return static_cast<uint8_t>(1u << (slot - kMinBenchSlot)); }

void reportSlotMasks() {
  serialOut().print("[bench] slots forced on 0x");
  serialOut().print(gBench.slotsForcedOn, HEX);
  serialOut().print(", forced off 0x");
  serialOut().println(gBench.slotsOff, HEX);
}

void toggleKnobsPinned() {
  gBench.knobsPinned = !gBench.knobsPinned;
  serialOut().println(gBench.knobsPinned ? "[bench] knobs pinned at top" : "[bench] knobs live");
}

void pinKnob(const BenchArgs& args) {
  gBench.pinKnob(args.slot - kMinBenchSlot, args.knob - kMinBenchKnob, args.value);
  serialOut().print("[bench] slot ");
  serialOut().print(args.slot);
  serialOut().print(" knob ");
  serialOut().print(args.knob);
  serialOut().print(" pinned at ");
  serialOut().print(args.value);
  serialOut().println(" % of travel");
}

void releaseKnobs() {
  gBench.knobsLive();
  serialOut().println("[bench] knobs live");
}

void toggleReverbChasm() {
  gBench.reverbChasm = !gBench.reverbChasm;
  serialOut().println(gBench.reverbChasm ? "[bench] reverb slot runs Chasm"
                                    : "[bench] reverb slot runs its card's engine");
}

// Flips a forced state and prints which way it went, in the words the stress run reads.
void toggleForced(bool& flag, const char* forced, const char* released) {
  flag = !flag;
  serialOut().println(flag ? forced : released);
}

// Null on success; turning the clip on fails when none is compiled in.
const char* toggleClip() {
  const bool wantOn = !audioBenchClipOn();
  const bool on = audioBenchClip(wantOn);
  serialOut().println(on ? "[bench] vocal clip on" : "[bench] vocal clip off");
  return wantOn && !on ? "no clip compiled in" : nullptr;
}

void resetPeaks() {
  audioResetPeaks();
  serialOut().println("[bench] audio peaks reset");
}

void stopAudio() {
  serialOut().println("[bench] stopping audio DMA");
  audioStopForBench();
}

void raiseStreamFault() {
  serialOut().println("[bench] raising a stream fault");
  audioStreamFaultForBench();
}

void toggleFailRestart() {
  serialOut().println(audioBenchToggleFailRestart() ? "[bench] the next stream restart fails"
                                                    : "[bench] stream restarts run as usual");
}

// Returns an error text when no bootloader wrote the block the word lives in.
const char* failNextAudioStart() {
  if (!audioBenchFailNextStart()) return "boot block not written";
  serialOut().println("[bench] the next audioInit() fails, once, across the next reset");
  return nullptr;
}

void burn(const BenchArgs& args) {
  audioBenchBurn(static_cast<uint32_t>(args.cycles), static_cast<uint32_t>(args.blocks), args.slot - 1);
  serialOut().print("[bench] burn ");
  serialOut().print(args.cycles);
  serialOut().print(" cycles");
  if (args.cycles > 0 && args.slot > 0) {
    serialOut().print(" in slot ");
    serialOut().print(args.slot);
  }
  if (args.cycles > 0 && args.blocks > 0) {
    serialOut().print(" for ");
    serialOut().print(args.blocks);
    serialOut().print(" blocks");
  }
  serialOut().println();
}

}  // namespace

const BenchOverrides& benchOverrides() { return gBench; }

BenchResult benchDrillRun(BenchOp op, const BenchArgs& args) {
  BenchResult r;
  switch (op) {
    case BenchOp::SlotOff:
      gBench.slotsOff ^= slotBit(args.slot);
      reportAudioStats();
      r.republish = true;
      break;
    case BenchOp::SlotsOn:
      gBench.slotsOff = 0;
      reportAudioStats();
      r.republish = true;
      break;
    case BenchOp::SlotOn:
      gBench.slotsForcedOn ^= slotBit(args.slot);
      reportSlotMasks();
      r.republish = true;
      break;
    case BenchOp::AllOn:
      gBench.slotsForcedOn = kAllSlotsMask;
      reportSlotMasks();
      r.republish = true;
      break;
    case BenchOp::SlotsLive:
      gBench.slotsLive();
      reportSlotMasks();
      r.republish = true;
      break;
    case BenchOp::PinKnobs: toggleKnobsPinned(); r.republish = true; break;
    case BenchOp::SetKnob: pinKnob(args); r.republish = true; break;
    case BenchOp::KnobsLive: releaseKnobs(); r.republish = true; break;
    case BenchOp::Chasm: toggleReverbChasm(); r.republish = true; break;
    case BenchOp::Bypass:
      toggleForced(gBench.bypassOn, "[bench] BYPASS held on", "[bench] BYPASS follows the top switch");
      break;
    case BenchOp::CleanupEq:
      toggleForced(gBench.eqOff, "[bench] CLEANUP EQ forced off", "[bench] CLEANUP EQ follows the menu");
      r.republish = true;
      break;
    case BenchOp::Clip: r.error = toggleClip(); break;
    case BenchOp::ResetPeaks: resetPeaks(); break;
    case BenchOp::StopAudio: stopAudio(); break;
    case BenchOp::StreamFault: raiseStreamFault(); break;
    case BenchOp::FailRestart: toggleFailRestart(); break;
    case BenchOp::FailAudioStart: r.error = failNextAudioStart(); break;
    case BenchOp::Burn: burn(args); break;
    case BenchOp::Fault: faultForceBusFault(); break;
    case BenchOp::Spin: faultSpinForWatchdog(); break;
    case BenchOp::RecordSave: recordSaveNow(); break;
    case BenchOp::RecordErase:
      if (!recordEraseAll()) r.error = "record store busy";
      break;
    case BenchOp::PanelTest:
    case BenchOp::PanelSummary:
    case BenchOp::PanelDump:
    case BenchOp::PanelJack:
    case BenchOp::PanelMute:
    case BenchOp::PanelOled: r.error = benchPanelRun(op); break;
    default: r.error = "op not handled"; break;
  }
  return r;
}

#endif
