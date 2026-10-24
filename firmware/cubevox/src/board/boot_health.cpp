#include "board/boot_health.h"

#include <Arduino.h>

#include "board/audio.h"
#include "board/boot_block_access.h"
#include "core/boot_health_rule.h"

namespace {

uint32_t gFailsAtBoot = 0;
uint32_t gReasonAtBoot = CvReasonNone;
uint32_t gSetupDoneMs = 0;
uint32_t gBlocksAtSetup = 0;
bool gOpen = false;
bool gReported = false;

}  // namespace

void bootHealthInit() {
  const CvBootBlock& block = bootBlock();
  gFailsAtBoot = failedStartsBefore(cvBootFailsGet(&block));
  // Another block version means the bootloader did not write this one since power-on.
  gReasonAtBoot = block.blockVersion == CV_BOOT_BLOCK_VERSION ? block.lastReason : static_cast<uint32_t>(CvReasonNone);
  gSetupDoneMs = millis();
  gBlocksAtSetup = audioBlockCount();
  gOpen = true;
}

void bootHealthPoll(uint32_t nowMs) {
  if (!gOpen || gReported) return;
  if (!bootHealthy(gSetupDoneMs, nowMs, gBlocksAtSetup, audioBlockCount(), CV_HEALTHY_MS)) return;
  cvBootFailsSet(&bootBlock(), 0u);
  gReported = true;
}

uint32_t bootHealthFails() { return gFailsAtBoot; }

const char* bootHealthLastReason() { return updateReasonName(gReasonAtBoot); }
