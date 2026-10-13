// Bench-class ops that act on the box: slot and knob overrides, audio stop, forced faults, record
// save and erase, the panel test. Bench builds only; the release image gets inline stubs and none
// of the code.

#pragma once

#include "board/build_kind.h"
#include "core/bench_ops.h"
#include "core/compose.h"

// What an op did. A null `error` means it succeeded; `republish` asks the caller to republish the
// snapshot because the overrides changed.
struct BenchResult {
  bool republish = false;
  const char* error = nullptr;
};

#if CUBEVOX_BENCH

const BenchOverrides& benchOverrides();

// Runs one bench-class op. `slot` is the slot_off target, 1 to 8.
BenchResult benchDrillRun(BenchOp op, int slot);

#else

inline const BenchOverrides& benchOverrides() {
  static const BenchOverrides none;
  return none;
}

inline BenchResult benchDrillRun(BenchOp, int) { return {false, "op not in this build"}; }

#endif
