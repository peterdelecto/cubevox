// Bench operations of the serial protocol (spec 2026-10-12-serial-protocol-design.md, "Bench
// operations"). One table holds every op's wire name and class.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/slots.h"

// Table index. Count ends the list.
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
  Count,
};

constexpr int kBenchOpCount = static_cast<int>(BenchOp::Count);

// Bytes of an op name including the NUL. A name read from the wire is clipped to this, so every
// real name is at least two characters shorter than it.
constexpr size_t kOpNameBytes = 16;

// Query ops are read-only and exist in every build. Bench ops act on the box and exist only in
// bench builds.
enum class OpClass : uint8_t { Query, Bench };

constexpr int kMinBenchSlot = 1;  // slot_off takes the panel numbering
constexpr int kMaxBenchSlot = kSlotCount;

// The op at table index `index`; BenchOp::Count when the index is out of range.
BenchOp benchOp(int index);

// "" for BenchOp::Count.
const char* opName(BenchOp op);

// Bench for BenchOp::Count, so an invalid op never counts as a query.
OpClass opClass(BenchOp op);

// Looks up an op by its wire name.
bool opFromName(const char* name, BenchOp& out);

// Queries run everywhere; bench ops run only when `benchBuild` is true.
bool opAllowed(BenchOp op, bool benchBuild);
