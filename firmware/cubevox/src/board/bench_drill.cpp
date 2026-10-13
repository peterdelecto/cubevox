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
bool gClipOn = false;

void toggleKnobsPinned() {
  gBench.knobsPinned = !gBench.knobsPinned;
  serialOut().println(gBench.knobsPinned ? "[bench] knobs pinned at top" : "[bench] knobs live");
}

void toggleReverbChasm() {
  gBench.reverbChasm = !gBench.reverbChasm;
  serialOut().println(gBench.reverbChasm ? "[bench] reverb slot runs Chasm"
                                    : "[bench] reverb slot runs its card's engine");
}

// Null on success; turning the clip on fails when none is compiled in.
const char* toggleClip() {
  const bool wantOn = !gClipOn;
  gClipOn = audioBenchClip(wantOn);
  serialOut().println(gClipOn ? "[bench] vocal clip on" : "[bench] vocal clip off");
  return wantOn && !gClipOn ? "no clip compiled in" : nullptr;
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

BenchResult benchDrillRun(BenchOp op, int slot) {
  BenchResult r;
  switch (op) {
    case BenchOp::SlotOff:
      gBench.slotsOff ^= static_cast<uint8_t>(1u << (slot - kMinBenchSlot));
      benchPrintStats();
      r.republish = true;
      break;
    case BenchOp::SlotsOn:
      gBench.slotsOff = 0;
      benchPrintStats();
      r.republish = true;
      break;
    case BenchOp::PinKnobs: toggleKnobsPinned(); r.republish = true; break;
    case BenchOp::Chasm: toggleReverbChasm(); r.republish = true; break;
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
