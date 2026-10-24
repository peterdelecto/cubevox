#include "board/record_service.h"

#include "board/boot_block_access.h"
#include "board/fault.h"
#include "board/flash_bank2.h"
#include "board/serial_link.h"
#include "core/protocol.h"
#include "core/record_store.h"

namespace {

Bank2Flash& gFlash = bank2Flash();
RecordStore gStore(gFlash);

bool gLoaded = false;
uint32_t gBootSequence = 0;
int gBootSector = 0;
int gEccSector = -1;      // sector erased by ECC recovery, -1 when none
uint32_t gEccAddress = 0;
bool gEccEraseFailed = false;
bool gFaultInitMissing = false;
bool gLayoutRejected = false;
char gRejectReason[protocol::kErrorBytes] = {};

// The bank 2 address of a double ECC error. Either the last run hit it, or the bootloader hit it
// reading the diagnostic log for DIAG.TXT and left the address in the boot block. The bootloader
// reads no other bank 2 sector, so no other address from it is acted on.
bool eccFaultAddress(uint32_t& address) {
  if (faultLastWasBank2Ecc(address)) return true;
  if (!bootBlockWritten()) return false;
  const uint32_t reported = bootBlock().faultAddress;
  if (bank2RecordSector(reported) != kDiagLogSector) return false;
  address = reported;
  return true;
}

// A double ECC error on a half-programmed word repeats on every read, so the sector goes. That
// includes sector 2, the diagnostic log, whose scan at boot would otherwise fault again.
void recoverEcc() {
  if (!eccFaultAddress(gEccAddress)) return;
  gEccSector = bank2RecordSector(gEccAddress);
  if (gEccSector < 0) return;
  gFlash.eraseBlocking(gEccSector);
  gEccEraseFailed = gFlash.takeError();
}

void printSettings(Print& out, const Settings& s) {
  out.print(" output ");
  out.print(s.outputLevel == OutputLevel::Instrument ? "instrument" : "line");
  out.print(" jack ");
  out.print(s.inputJack == InputJack::Pedal ? "pedal" : "mic");
}

void reportRecovery(Print& out) {
  if (gFaultInitMissing) out.println("[ERROR] record: recordBoot ran before faultInit, ECC recovery skipped");
  if (gEccSector >= 0) {
    out.print(gEccEraseFailed ? "[ERROR] bank 2 ECC fault at 0x" : "[WARN] bank 2 ECC fault at 0x");
    out.print(gEccAddress, HEX);
    out.print(", sector ");
    out.print(gEccSector);
    out.println(gEccEraseFailed ? " erase failed" : " erased");
  }
  if (gLayoutRejected) {
    out.print("[WARN] record layout invalid (");
    out.print(gRejectReason);
    out.println("), using the default layout");
  }
}

void reportScan(Print& out) {
  const BootNotes& notes = gStore.bootNotes();
  if (notes.unknownCard) out.println("[WARN] record: unknown card id, that slot is empty");
  if (notes.dirtySectors > 0) {
    out.print("[WARN] record: ");
    out.print(notes.dirtySectors);
    out.println(" sector(s) hold data after a blank entry, from an interrupted erase");
  }
  if (notes.tornEntries > 0) {
    out.print("[WARN] record: ");
    out.print(notes.tornEntries);
    out.println(" entries cut off mid-write");
  }
}

}  // namespace

void recordBoot(Layout& layout, Settings& settings, uint32_t nowMs) {
  // The recovery reads the fault record that faultInit() loads; before that it would see no fault.
  gFaultInitMissing = !faultInitDone();
  recoverEcc();
  RecordData rec;
  if (!gStore.begin(rec)) return;

  gBootSequence = rec.sequence;
  gBootSector = gStore.status().sector;
  gLoaded = true;
  layout = rec.layout;
  settings = rec.settings;
  const Recovery recovery = recoverLayout(layout);
  gLayoutRejected = recovery.replaced;
  if (!recovery.replaced) return;
  layoutReason(recovery.why, gRejectReason, sizeof gRejectReason);
  gStore.request(layout, settings, nowMs);  // rewrite the record with the default layout
}

void recordRequestSave(const Layout& layout, const Settings& settings, uint32_t nowMs) {
  gStore.request(layout, settings, nowMs);
}

void recordSetName(const char* name) { gStore.setName(name); }

const char* recordName() { return gStore.current().name; }

uint32_t recordSequence() { return gStore.status().sequence; }

uint32_t recordErrors() { return gStore.status().errors; }

void recordPoll(uint32_t nowMs) {
  // The bank takes one operation at a time, so a save waits while the diagnostic log erases.
  if (gFlash.erasingSector() != kDiagLogSector) gStore.poll(nowMs);
  if (gStore.takeSaveFailed()) serialOut().println("[ERROR] record save failed, the box keeps the previous record");
}

bool recordFlush(uint32_t timeoutMs) {
  gStore.flushPending();
  const uint32_t startMs = millis();
  while (gStore.status().busy) {
    if (millis() - startMs >= timeoutMs) {
      serialOut().println("[WARN] record save still running at reset");
      return false;
    }
    faultWatchdogKick();
    recordPoll(millis());
  }
  return true;
}

#if CUBEVOX_BENCH
void recordSaveNow() {
  gStore.saveNow();
  serialOut().println("[bench] record save requested");
}

bool recordEraseAll() {
  const bool started = gStore.eraseAll();
  serialOut().println(started ? "[bench] erasing both bank 2 sectors" : "[bench] record store busy");
  return started;
}
#endif

void recordReportStatus(Print& out) {
  const RecordStatus st = gStore.status();
  out.print("[record] ");
  out.print(st.hasRecord ? "newest seq " : "none, seq ");
  out.print(st.sequence);
  out.print(" next sector ");
  out.print(st.sector);
  out.print(" entry ");
  out.print(st.nextEntry);
  out.print(" busy ");
  out.print(st.busy ? "yes" : "no");
  out.print(" errors ");
  out.println(st.errors);
  const RecordData& cur = gStore.current();
  out.print("[record] name \"");
  out.print(cur.name);
  out.print("\"");
  printSettings(out, cur.settings);
  out.println();
}

void recordReportBoot(Print& out) {
  reportRecovery(out);
  reportScan(out);
  if (gLoaded) {
    out.print("[boot] record: seq ");
    out.print(gBootSequence);
    out.print(" sector ");
    out.println(gBootSector);
  } else {
    out.println("[boot] record: none, defaults");
  }
}
