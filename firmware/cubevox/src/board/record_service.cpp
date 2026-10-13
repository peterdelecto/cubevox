#include "board/record_service.h"

#include "board/fault.h"
#include "board/flash_bank2.h"
#include "board/serial_link.h"
#include "core/record_store.h"

namespace {

Bank2Flash gFlash;
RecordStore gStore(gFlash);

bool gLoaded = false;
uint32_t gBootSequence = 0;
int gBootSector = 0;
int gEccSector = -1;      // sector erased by ECC recovery, -1 when none
uint32_t gEccAddress = 0;
bool gLayoutRejected = false;
char gRejectReason[48] = {};

// A double ECC error on a half-programmed word repeats on every read, so the sector goes.
void recoverEcc() {
  if (!faultLastWasBank2Ecc(gEccAddress)) return;
  gEccSector = bank2RecordSector(gEccAddress);
  if (gEccSector >= 0) gFlash.eraseBlocking(gEccSector);
}

void printSettings(Print& out, const Settings& s) {
  out.print(" output ");
  out.print(s.output == OutputLevel::Instrument ? "instrument" : "line");
  out.print(" jack ");
  out.print(s.inputJack == InputJack::Pedal ? "pedal" : "mic");
}

}  // namespace

void recordBoot(Layout& layout, Settings& settings) {
  recoverEcc();
  RecordData rec;
  if (!gStore.begin(rec)) return;

  gBootSequence = rec.sequence;
  gBootSector = gStore.status().sector;
  gLoaded = true;
  const LayoutCheck check = layoutValid(rec.layout);
  gLayoutRejected = !check.ok();
  if (gLayoutRejected) layoutReason(check, gRejectReason, sizeof gRejectReason);
  if (!gLayoutRejected) layout = rec.layout;
  settings = rec.settings;
  if (gLayoutRejected) gStore.request(layout, settings, millis());  // rewrite with the default layout
}

void recordRequestSave(const Layout& layout, const Settings& settings, uint32_t nowMs) {
  gStore.request(layout, settings, nowMs);
}

void recordSetName(const char* name) { gStore.setName(name); }

const char* recordName() { return gStore.current().name; }

uint32_t recordSequence() { return gStore.status().sequence; }

uint32_t recordErrors() { return gStore.status().errors; }

void recordPoll(uint32_t nowMs) { gStore.poll(nowMs); }

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

void recordPrintStatus(Print& out) {
  const RecordStatus st = gStore.status();
  out.print("[record] ");
  out.print(st.hasRecord ? "newest seq " : "none, seq ");
  out.print(st.sequence);
  out.print(" next sector ");
  out.print(st.sector);
  out.print(" slot ");
  out.print(st.nextSlot);
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

void recordPrintBoot(Print& out) {
  if (gEccSector >= 0) {
    out.print("[WARN] bank 2 ECC fault at 0x");
    out.print(gEccAddress, HEX);
    out.print(", sector ");
    out.print(gEccSector);
    out.println(" erased");
  }
  if (gLayoutRejected) {
    out.print("[WARN] record layout invalid (");
    out.print(gRejectReason);
    out.println("), using the default layout");
  }
  if (gLoaded) {
    out.print("[boot] record: seq ");
    out.print(gBootSequence);
    out.print(" sector ");
    out.println(gBootSector);
  } else {
    out.println("[boot] record: none, defaults");
  }
}
