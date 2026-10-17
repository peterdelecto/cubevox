// Bench operations from the serial protocol ({"cmd":"bench","op":"<name>"}, spec 2026-10-12
// serial protocol, "Bench operations"). Query ops exist in every build; the bench class (drills,
// overrides, panel test) exists only in bench builds and lives in bench_drill and bench_panel.

#pragma once

#include "board/bench_drill.h"
#include "core/bench_ops.h"

// Runs one op the parser already accepted for this build. `slot` is the slot_off target.
// `bootReport` prints the boot report (op report). The result says whether it failed and whether
// the caller must republish the snapshot.
BenchResult benchRun(BenchOp op, int slot, void (*bootReport)());

// Prints the audio counters, total and per slot.
void benchReportStats();
