#include "board/bench.h"

#include <Arduino.h>

#include "board/audio.h"
#include "board/record_service.h"
#include "board/serial_link.h"
#include "core/layout.h"

namespace {

void printSlotRow(const char* label, const uint32_t* values) {
  serialOut().print(label);
  for (int i = 0; i <= kSlotCount; ++i) {
    serialOut().print(' ');
    serialOut().print(values[i]);
  }
}

}  // namespace

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
  serialOut().println(benchOverrides().slotsOff, HEX);
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

BenchResult benchRun(BenchOp op, int slot, void (*bootReport)()) {
  switch (op) {
    case BenchOp::Report: bootReport(); return {};
    case BenchOp::Stats: benchPrintStats(); return {};
    case BenchOp::RecordStatus: recordPrintStatus(serialOut()); return {};
    case BenchOp::SlotOff:
    case BenchOp::SlotsOn:
    case BenchOp::PinKnobs:
    case BenchOp::Chasm:
    case BenchOp::Clip:
    case BenchOp::ResetPeaks:
    case BenchOp::StopAudio:
    case BenchOp::Fault:
    case BenchOp::Spin:
    case BenchOp::RecordSave:
    case BenchOp::RecordErase:
    case BenchOp::PanelTest:
    case BenchOp::PanelSummary:
    case BenchOp::PanelDump:
    case BenchOp::PanelJack:
    case BenchOp::PanelMute:
    case BenchOp::PanelOled: return benchDrillRun(op, slot);
  }
  return {false, "op not handled"};
}
