#include "board/dfu.h"

#include <Arduino.h>

#include "board/boot_block_access.h"
#include "board/diag_service.h"
#include "board/fault.h"
#include "board/flash_bank2.h"
#include "board/mute.h"
#include "board/record_service.h"
#include "board/serial_link.h"
#include "core/version.h"

namespace {

// A stuck erase ends this wait when the flash driver's erase timeout does.
constexpr uint32_t kSaveWaitMs = kEraseTimeoutMs;

// Eight flash words take well under a second. An erase the entry needs is waited out beyond this.
constexpr uint32_t kDiagWaitMs = 3000;

// The bootloader prints this field in DIAG.TXT: the version, the uptime in seconds, this boot's
// reset cause and the last fault kind, as in "v2.00.10 3600s power-on none". The field holds 40
// bytes, so a longer text loses its tail and stays NUL-terminated.
void writeSummary(CvBootBlock& block) {
  char tag[16];
  versionTag(kFirmwareVersion, tag, sizeof(tag));
  char causes[24];
  faultSummaryText(causes, sizeof(causes));
  memset(block.appSummary, 0, sizeof(block.appSummary));
  const int written = snprintf(block.appSummary, sizeof(block.appSummary), "%s %lus %s", tag,
                               static_cast<unsigned long>(millis() / 1000), causes);
  if (written < 0) block.appSummary[0] = '\0';
}

}  // namespace

void enterDfu() {
  CvBootBlock& block = bootBlock();
  writeSummary(block);
  diagNoteUpdateRequest();
  diagFlush(kDiagWaitMs);  // a timeout prints its own warning, and the update goes ahead
  recordFlush(kSaveWaitMs);
  serialFlush(kFlushBeforeResetMs);
  muteForReset();
  block.request = CV_REQUEST_UPDATE;
  __DSB();  // the request must reach DTCM before the reset
  NVIC_SystemReset();
}
