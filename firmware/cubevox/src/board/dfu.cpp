#include "board/dfu.h"

#include <Arduino.h>

#include <array>

#include "board/boot_block_access.h"
#include "board/diag_service.h"
#include "board/fault.h"
#include "board/planned_reset.h"
#include "core/version.h"

namespace {

bool gRequested = false;

// The bootloader prints this field in DIAG.TXT: the version, the uptime in seconds, this boot's
// reset cause and the last fault kind, as in "v2.00.10 3600s power-on none". The field holds 40
// bytes, so a longer text loses its tail and stays NUL-terminated.
void writeSummary(CvBootBlock& block) {
  std::array<char, 24> causes{};
  faultResetText(causes.data(), causes.size());
  memset(block.appSummary, 0, sizeof(block.appSummary));
  const int written = snprintf(block.appSummary, sizeof(block.appSummary), "%s %lus %s", kFirmwareVersion,
                               static_cast<unsigned long>(millis() / 1000), causes.data());
  if (written < 0) block.appSummary[0] = '\0';
}

}  // namespace

void enterDfu(uint32_t nowMs) {
  if (gRequested) return;
  gRequested = true;
  writeSummary(bootBlock());
  diagNoteUpdateRequest();
  plannedResetStart(PlannedReset::Update, nowMs);
}
