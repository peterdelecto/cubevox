#include "board/record_service.h"

#include <array>

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
int gEccSector = -1;  // sector the ECC recovery handled, -1 when none
uint32_t gEccAddress = 0;
RecordRecovery gEccRecord;     // what the recovery did in a record sector
bool gLogEraseFailed = false;  // the log's sector would not erase
bool gFaultInitMissing = false;
bool gLayoutRejected = false;
std::array<char, protocol::kErrorBytes> gRejectReason = {};
uint32_t gBootFaultAddress = 0;  // the bootloader's last read fault, kept after recoverEcc clears it

// The bank 2 address of a double ECC error. Either the last run hit it, or the bootloader hit it
// (reading the diagnostic log for DIAG.TXT, say) and left the address in the boot block.
bool eccFaultAddress(uint32_t& address) {
  if (faultLastWasBank2Ecc(address)) return true;
  if (bank2SectorOf(gBootFaultAddress) < 0) return false;
  address = gBootFaultAddress;
  return true;
}

// A double ECC error on a half-programmed word repeats on every read, so the faulted sector goes.
// In a record sector the store first carries the newest readable record to the other sector. The
// log's sector holds diagnostics alone and is simply erased; its scan at boot would otherwise fault
// again. Once the sector the bootloader named is erased, its address is cleared so the bootloader
// reads the log again. A sector that stays leaves the address, and the bootloader keeps skipping it.
void recoverEcc() {
  gBootFaultAddress = bootBlockWritten() ? bootBlock().faultAddress : 0u;
  if (!eccFaultAddress(gEccAddress)) return;
  gEccSector = bank2SectorOf(gEccAddress);
  if (gEccSector < 0) return;
  bool erased = false;
  if (gEccSector == kDiagLogSector) {
    gLogEraseFailed = !eraseAndWait(gFlash, gEccSector);
    erased = !gLogEraseFailed;
  } else {
    gEccRecord = gStore.recover(gEccSector, (gEccAddress - kBank2Base) % kFlashSectorBytes);
    erased = !gEccRecord.carryFailed && !gEccRecord.eraseFailed;
  }
  if (erased && bank2SectorOf(gBootFaultAddress) == gEccSector) bootBlock().faultAddress = 0;
}

// One line on what the recovery did, an [ERROR] when a sector had to stay.
void reportEcc(Print& out) {
  const RecordRecovery& r = gEccRecord;
  out.print(gLogEraseFailed || r.carryFailed || r.eraseFailed ? "[ERROR]" : "[WARN]");
  out.print(" bank 2 ECC fault at 0x");
  out.print(gEccAddress, HEX);
  out.print(" in sector ");
  out.print(gEccSector);
  if (r.carried || r.carryFailed) {
    out.print("; record seq ");
    out.print(r.sequence);
    out.print(r.carried ? " moved to sector " : " could not move to sector ");
    out.print(1 - gEccSector);
  }
  if (r.carryFailed) {
    out.println(", so the sector stays and is read up to the fault");
  } else if (gLogEraseFailed) {
    out.println("; it would not erase, so the diag log stays closed");
  } else {
    out.println(r.eraseFailed ? "; it would not erase and stays unread" : "; it is erased");
  }
}

void printSettings(Print& out, const Settings& s) {
  out.print(" output ");
  out.print(s.outputLevel == OutputLevel::Mic ? "mic" : "line");
  out.print(" jack ");
  out.print(s.inputJack == InputJack::Line ? "line" : "mic");
}

void reportRecovery(Print& out) {
  if (gFaultInitMissing) out.println("[ERROR] record: recordBoot ran before faultInit, ECC recovery skipped");
  if (gEccSector >= 0) reportEcc(out);
  if (gLayoutRejected) {
    out.print("[WARN] record layout invalid (");
    out.print(gRejectReason.data());
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
  layoutReason(recovery.why, gRejectReason.data(), gRejectReason.size());
  gStore.request(layout, settings, nowMs);  // rewrite the record with the default layout
}

void recordRequestSave(const Layout& layout, const Settings& settings, uint32_t nowMs) {
  gStore.request(layout, settings, nowMs);
}

void recordSkipDebounce() { gStore.flushPending(); }

bool recordBusy() { return gStore.status().busy; }

bool recordLogSectorReadable() { return !gLogEraseFailed; }

void recordSetName(const char* name) { gStore.setName(name); }

const char* recordName() { return gStore.current().name; }

uint32_t recordSequence() { return gStore.status().sequence; }

uint32_t recordErrors() { return gStore.status().errors; }

uint32_t recordBootFaultAddress() { return gBootFaultAddress; }

void recordPoll(uint32_t nowMs) {
  // The bank takes one operation at a time, so a save waits while the diagnostic log erases.
  if (gFlash.erasingSector() != kDiagLogSector) gStore.poll(nowMs);
  if (gStore.takeSaveFailed()) serialOut().println("[ERROR] record save failed, the box keeps the previous record");
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
