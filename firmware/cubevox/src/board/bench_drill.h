// Bench-class ops that act on the box: slot and knob overrides, audio stop, forced faults, record
// save and erase, the panel test. Bench builds only; the release image gets the empty overrides
// and none of the code.

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

// Runs one bench-class op. `args` holds the slot, knob and value the op takes, already range-checked
// by the parser.
BenchResult benchDrillRun(BenchOp op, const BenchArgs& args);

#else

inline const BenchOverrides& benchOverrides() {
  static const BenchOverrides none;
  return none;
}

#endif
