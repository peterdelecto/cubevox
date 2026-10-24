#include "board/diag_service.h"

#include <stdint.h>
#include <string.h>

#include "board/audio.h"
#include "board/boot_block_access.h"
#include "board/boot_health.h"
#include "board/fault.h"
#include "board/flash_bank2.h"
#include "board/record_service.h"
#include "board/serial_link.h"
#include "core/entry_log.h"
#include "core/version.h"

namespace {

// The layout record owns the controller while it erases. The log's own erase is polled by the log.
bool bankFree() {
  const int erasing = bank2Flash().erasingSector();
  return erasing < 0 || erasing == kDiagLogSector;
}

EntryLog gLog(bank2Flash(), kDiagLogSector, CV_DIAG_ENTRY_SIZE, offsetof(CvDiagEntry, sequence), bankFree);
bool gFailed = false;  // the log would not open, or a write failed since boot

uint32_t nowMs() { return millis(); }

// Copies at most srcMax bytes of `src`, up to its first NUL, and leaves the last byte of `dest`
// NUL. `dest` starts zeroed.
void copyText(char* dest, size_t destSize, const char* src, size_t srcMax) {
  for (size_t i = 0; i + 1 < destSize && i < srcMax && src[i] != '\0'; ++i) dest[i] = src[i];
}

// The fields both kinds of entry share. The log stamps the sequence and the CRC. Text fields are
// NUL padded and reserved bytes stay 0xFF, as boot_block.h says.
CvDiagEntry makeEntry(CvDiagKind kind) {
  CvDiagEntry e;
  memset(&e, 0, sizeof e);
  memset(e.reserved, 0xFF, sizeof e.reserved);
  e.magic = CV_DIAG_MAGIC;
  e.kind = kind;
  e.uptimeS = CV_DIAG_UNKNOWN_U32;
  e.resetCause = faultResetFlags();
  e.bootFails = bootHealthFails();
  e.lastReason = bootHealthLastReasonCode();
  e.faultAddress = recordBootFaultAddress();
  e.saiErrors = audioErrors().count;
  e.overruns = audioStats().overruns;
  copyText(e.firmware, sizeof e.firmware, kFirmwareVersion, SIZE_MAX);
  if (faultRecordPresent()) faultSummaryText(e.fault, sizeof e.fault);
  return e;
}

void queue(const CvDiagEntry& entry) { gLog.append(reinterpret_cast<const uint8_t*>(&entry)); }

void reportFailure() {
  if (!gLog.takeError()) return;
  gFailed = true;
  serialOut().println("[ERROR] diag log write failed, that entry is lost");
}

// A start that followed a plain power-on with no fault record has nothing to explain.
void queueBootEntry() {
  if (faultResetWasPowerOn() && !faultRecordPresent()) return;
  CvDiagEntry e = makeEntry(CvDiagKindBoot);
  if (bootBlockWritten()) {
    const CvBootBlock& block = bootBlock();
    copyText(e.summary, sizeof e.summary, block.appSummary, sizeof block.appSummary);
  }
  queue(e);
}

}  // namespace

void diagInit() {
  gFailed = !gLog.begin();
  if (!gFailed) queueBootEntry();
}

void diagPoll() {
  gLog.poll();
  reportFailure();
}

void diagNoteUpdateRequest() {
  CvDiagEntry e = makeEntry(CvDiagKindUpdateRequest);
  e.uptimeS = millis() / 1000;
  queue(e);
}

bool diagFlush(uint32_t timeoutMs) {
  const bool done = gLog.flush(timeoutMs, nowMs, faultWatchdogKick);
  reportFailure();
  if (!done) serialOut().println("[WARN] diag entry still unwritten at reset");
  return done;
}

void diagReportBoot(Print& out) {
  if (gFailed) {
    out.println("[ERROR] diag log: error");
  } else if (gLog.count() == 0) {
    out.println("[boot] diag log: empty");
  } else {
    out.print("[boot] diag log: ");
    out.print(gLog.count());
    out.print(" entries, last seq ");
    out.println(gLog.lastSequence());
  }
}
