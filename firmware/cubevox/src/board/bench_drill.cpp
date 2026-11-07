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
    case BenchOp::PolishComp:
      toggleForced(gBench.compOff, "[bench] POLISH COMP forced off", "[bench] POLISH COMP follows the menu");
      r.republish = true;
      break;
    case BenchOp::PolishEq:
      toggleForced(gBench.eqOff, "[bench] VOX EQ forced off", "[bench] VOX EQ follows the menu");
      r.republish = true;
      break;
    case BenchOp::Clip: r.error = toggleClip(); break;
    case BenchOp::ResetPeaks: resetPeaks(); break;
    case BenchOp::StopAudio: stopAudio(); break;
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
