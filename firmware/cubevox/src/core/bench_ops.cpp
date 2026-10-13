#include "core/bench_ops.h"

#include <string.h>

namespace {

struct OpEntry {
  BenchOp op;
  const char* name;
  OpClass cls;
};

// Indexed by BenchOp value.
constexpr OpEntry kOps[kBenchOpCount] = {
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
};

}  // namespace

BenchOp benchOp(int index) { return kOps[index].op; }

const char* opName(BenchOp op) { return kOps[static_cast<int>(op)].name; }

OpClass opClass(BenchOp op) { return kOps[static_cast<int>(op)].cls; }

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
