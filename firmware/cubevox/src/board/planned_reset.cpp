#include "board/planned_reset.h"

#include <Arduino.h>

#include "board/boot_block_access.h"
#include "board/diag_service.h"
#include "board/flash_bank2.h"
#include "board/mute.h"
#include "board/record_service.h"
#include "board/serial_link.h"
#include "core/loop_policy.h"

namespace {

bool gPending = false;
PlannedReset gWhy = PlannedReset::AudioStall;
ResetWait gWait;

void reportUnfinished() {
  if (recordBusy()) serialOut().println("[WARN] record save still running at reset");
  if (diagBusy()) serialOut().println("[WARN] diag entry still unwritten at reset");
}

[[noreturn]] void resetNow() {
  serialFlush(kFlushBeforeResetMs);
  muteForReset();
  if (gWhy == PlannedReset::Update) {
    bootBlock().request = CV_REQUEST_UPDATE;
    __DSB();  // the request must reach DTCM before the reset
  }
  NVIC_SystemReset();
}

}  // namespace

void plannedResetStart(PlannedReset why, uint32_t nowMs) {
  if (!gPending || why == PlannedReset::Update) gWhy = why;
  if (gPending) return;
  gPending = true;
  gWait.start(nowMs);
}

bool plannedResetPending() { return gPending; }

void plannedResetPoll(uint32_t nowMs) {
  if (!gPending) return;
  recordSkipDebounce();  // a change made during the wait goes out at once
  const bool erasing = bank2Flash().erasingSector() >= 0;
  switch (gWait.update(nowMs, recordBusy() || diagBusy(), erasing)) {
    case ResetWait::Verdict::Wait: return;
    case ResetWait::Verdict::GiveUp: reportUnfinished(); break;
    case ResetWait::Verdict::Go: break;
  }
  resetNow();
}
