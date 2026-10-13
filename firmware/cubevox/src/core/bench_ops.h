// Bench operations of the serial protocol (spec 2026-10-12-serial-protocol-design.md, "Bench
// operations"). One table holds every op's wire name and class.

#pragma once

#include <stdint.h>

enum class BenchOp : uint8_t {
  Report,
  Stats,
  RecordStatus,
  SlotOff,
  SlotsOn,
  PinKnobs,
  Chasm,
  Clip,
  ResetPeaks,
  StopAudio,
  Fault,
  Spin,
  RecordSave,
  RecordErase,
  PanelTest,
  PanelSummary,
  PanelDump,
  PanelJack,
  PanelMute,
  PanelOled,
};

constexpr int kBenchOpCount = 20;

// Query ops are read-only and exist in every build. Bench ops act on the box and exist only in
// bench builds.
enum class OpClass : uint8_t { Query, Bench };

constexpr int kMinBenchSlot = 1;  // slot_off takes the panel numbering
constexpr int kMaxBenchSlot = 8;

BenchOp benchOp(int index);
const char* opName(BenchOp op);
OpClass opClass(BenchOp op);

// Looks up an op by its wire name.
bool opFromName(const char* name, BenchOp& out);

// Queries run everywhere; bench ops run only when `benchBuild` is true.
bool opAllowed(BenchOp op, bool benchBuild);
