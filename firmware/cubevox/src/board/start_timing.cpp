#include "board/start_timing.h"

namespace {

// Each time is written once: the block time by the block interrupt, the others by loop(). Readers
// check the flag first, and the flag is set after the time.
struct Mark {
  volatile uint32_t ms = 0;
  volatile bool set = false;

  void note(uint32_t sinceBeginMs) {
    if (set) return;
    ms = sinceBeginMs;
    set = true;
  }
};

uint32_t gBeginMs = 0;
bool gWarm = false;
Mark gBlock;
Mark gSnapshot;
Mark gRelay;

void printMark(Print& out, const char* name, const Mark& mark) {
  out.print(", ");
  out.print(name);
  out.print(' ');
  if (mark.set) {
    out.print(mark.ms);
    out.print(" ms");
  } else {
    out.print("not yet");
  }
}

}  // namespace

void startTimingBegin(uint32_t nowMs) { gBeginMs = nowMs; }

void startTimingSetWarm(bool warm) { gWarm = warm; }

bool startIsWarm() { return gWarm; }

void startTimingNoteBlock() {
  if (!gBlock.set) gBlock.note(millis() - gBeginMs);
}

void startTimingNoteSnapshot(uint32_t nowMs) { gSnapshot.note(nowMs - gBeginMs); }

void startTimingNoteRelay(uint32_t nowMs) { gRelay.note(nowMs - gBeginMs); }

void startTimingReport(Print& out) {
  out.print("[boot] start: ");
  out.print(gWarm ? "warm" : "cold");
  if (gWarm) {
    printMark(out, "first block", gBlock);
    printMark(out, "first snapshot", gSnapshot);
    printMark(out, "relay", gRelay);
    out.print(" after setup() began");
  }
  out.println();
}
