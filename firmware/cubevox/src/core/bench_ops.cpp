#include "core/bench_ops.h"

#include <string.h>

#include <array>

namespace {

struct OpEntry {
  BenchOp op;
  const char* name;
  OpClass cls;
};

// Indexed by BenchOp value.
constexpr std::array<OpEntry, kBenchOpCount> kOps = {{
    {BenchOp::Report, "report", OpClass::Query},
    {BenchOp::Stats, "stats", OpClass::Query},
    {BenchOp::RecordStatus, "record_status", OpClass::Query},
    {BenchOp::SlotOff, "slot_off", OpClass::Bench},
    {BenchOp::SlotsOn, "slots_on", OpClass::Bench},
    {BenchOp::PinKnobs, "pin_knobs", OpClass::Bench},
    {BenchOp::Chasm, "chasm", OpClass::Bench},
    {BenchOp::Clip, "clip", OpClass::Bench},
    {BenchOp::ResetPeaks, "reset_peaks", OpClass::Bench},
    {BenchOp::StopAudio, "stop_audio", OpClass::Bench},
    {BenchOp::Fault, "fault", OpClass::Bench},
    {BenchOp::Spin, "spin", OpClass::Bench},
    {BenchOp::RecordSave, "record_save", OpClass::Bench},
    {BenchOp::RecordErase, "record_erase", OpClass::Bench},
    {BenchOp::PanelTest, "panel_test", OpClass::Bench},
    {BenchOp::PanelSummary, "panel_summary", OpClass::Bench},
    {BenchOp::PanelDump, "panel_dump", OpClass::Bench},
    {BenchOp::PanelJack, "panel_jack", OpClass::Bench},
    {BenchOp::PanelMute, "panel_mute", OpClass::Bench},
    {BenchOp::PanelOled, "panel_oled", OpClass::Bench},
}};

// Every entry sits at its own enum index, and every name fits the clip width with room to spare,
// so a clipped over-long name can never equal a real one.
constexpr bool tableIsSound() {
  for (int i = 0; i < kBenchOpCount; ++i) {
    const OpEntry& e = kOps[static_cast<size_t>(i)];
    if (static_cast<int>(e.op) != i || e.name == nullptr) return false;
    if (__builtin_strlen(e.name) + 2 > kOpNameBytes) return false;
  }
  return true;
}
static_assert(tableIsSound(), "kOps must follow the BenchOp enum and keep names short");

}  // namespace

BenchOp benchOp(int index) {
  return index >= 0 && index < kBenchOpCount ? kOps[static_cast<size_t>(index)].op : BenchOp::Count;
}

const char* opName(BenchOp op) {
  const size_t i = static_cast<size_t>(op);
  return i < kOps.size() ? kOps[i].name : "";
}

OpClass opClass(BenchOp op) {
  const size_t i = static_cast<size_t>(op);
  return i < kOps.size() ? kOps[i].cls : OpClass::Bench;
}

bool opFromName(const char* name, BenchOp& out) {
  for (const OpEntry& e : kOps) {
    if (strcmp(name, e.name) == 0) {
      out = e.op;
      return true;
    }
  }
  return false;
}

bool opAllowed(BenchOp op, bool benchBuild) { return benchBuild || opClass(op) == OpClass::Query; }
