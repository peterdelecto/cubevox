#include "board/bench.h"

#include <Arduino.h>

#include <array>

#include "board/audio.h"
#include "board/fault.h"
#include "board/record_service.h"
#include "board/serial_link.h"
#include "core/layout.h"

namespace {

// The eight slots in order, then the two polish stages and the input conditioning under their names.
void printSlotRow(const char* label, const std::array<uint32_t, kStatBuckets>& values) {
  serialOut().print(label);
  for (int s = 0; s < kSlotCount; ++s) {
    serialOut().print(' ');
    serialOut().print(values[s]);
  }
  serialOut().print(" COMP ");
  serialOut().print(values[kStatComp]);
  serialOut().print(" EQ ");
  serialOut().print(values[kStatEq]);
  serialOut().print(" COND ");
  serialOut().print(values[kStatCond]);
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
  serialOut().print(" dry ");
  serialOut().print(s.dryBlocks);
  serialOut().print(" avg ");
  serialOut().print(s.avgCycles);
  serialOut().print(" worst ");
  serialOut().print(s.maxCycles);
  serialOut().print(" of ");
  serialOut().print(s.budgetCycles);
  serialOut().print(" cycles, slots off 0x");
  serialOut().println(benchOverrides().slotsOff, HEX);
  printSlotRow("[audio] per slot", s.slotAvg);
  serialOut().println();
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
  serialOut().print("[stack] ");
  serialOut().print(faultStackUntouchedBytes());
  serialOut().println(" bytes never used since boot");
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
