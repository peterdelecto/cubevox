#include "board/bench.h"

#include <Arduino.h>

#include <array>

#include "board/audio.h"
#include "board/record_service.h"
#include "board/serial_link.h"
#include "core/layout.h"

namespace {

void printSlotRow(const char* label, const std::array<uint32_t, kSlotCount + 1>& values) {
  serialOut().print(label);
  for (uint32_t value : values) {
    serialOut().print(' ');
    serialOut().print(value);
  }
}

}  // namespace

void benchReportStats() {
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
    case BenchOp::Stats: benchReportStats(); return {};
    case BenchOp::RecordStatus: recordReportStatus(serialOut()); return {};
    default: break;  // bench-class ops
  }
#if CUBEVOX_BENCH
  return benchDrillRun(op, slot);
#else
  (void)slot;
  return {false, "op not handled"};  // the parser refuses these ops in a release build
#endif
}
