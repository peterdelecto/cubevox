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
  SlotOn,
  AllOn,
  SlotsLive,
  PinKnobs,
  SetKnob,
  KnobsLive,
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
  Bypass,
  CleanupEq,
  StreamFault,
  FailRestart,
  FailAudioStart,
  Count,
};

constexpr int kBenchOpCount = static_cast<int>(BenchOp::Count);

// Bytes of an op name including the NUL. A name read from the wire is clipped to this, so every
// real name is at least two characters shorter than it.
constexpr size_t kOpNameBytes = 20;

// Query ops are read-only and exist in every build. Bench ops act on the box and exist only in
// bench builds.
enum class OpClass : uint8_t { Query, Bench };

constexpr int kMinBenchSlot = 1;  // slot_off, slot_on and set_knob take the panel numbering
constexpr int kMaxBenchSlot = kSlotCount;

// set_knob names a knob 1 (upper) or 2 (lower) and a value in whole percent of the knob's travel.
constexpr int kMinBenchKnob = 1;
constexpr int kMaxBenchKnob = kKnobsPerCard;
constexpr int kMaxBenchKnobValue = 100;

// The arguments of a bench request, as the parser read them.
struct BenchArgs {
  int slot = 0;   // slot_off, slot_on, set_knob: 1 to kMaxBenchSlot
  int knob = 0;   // set_knob: 1 to kMaxBenchKnob
  int value = 0;  // set_knob: 0 to kMaxBenchKnobValue
};

// Which arguments an op needs. The parser requires and range-checks exactly these.
bool opTakesSlot(BenchOp op);
bool opTakesKnob(BenchOp op);  // knob and value

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
