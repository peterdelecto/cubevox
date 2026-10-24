// Checks the diagnostic log's flash side on the host: the append log in core/entry_log over the
// shared flash steps in core/flash_io, with the entry geometry of the bootloader contract in
// board/boot_block.h.

#include "support.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "board/boot_block.h"
#include "core/crc32.h"
#include "core/entry_log.h"
#include "fake_flash.h"

namespace {

constexpr int kLogSector = 2;
constexpr uint32_t kSlots = CV_DIAG_LOG_SIZE / CV_DIAG_ENTRY_SIZE;
constexpr size_t kSequenceAt = offsetof(CvDiagEntry, sequence);
constexpr int kWordsPerEntry = CV_DIAG_ENTRY_SIZE / kFlashWordBytes;

static_assert(CV_DIAG_LOG_SIZE == kFlashSectorBytes, "the contract sector is one flash sector");
static_assert(CV_DIAG_ENTRY_SIZE <= kMaxLogEntryBytes, "the log takes the contract entry");

bool gGateOpen = true;

bool gate() { return gGateOpen; }

EntryLog makeLog(FakeFlash& f, EntryLog::Gate g = nullptr) {
  return EntryLog(f, kLogSector, CV_DIAG_ENTRY_SIZE, kSequenceAt, g);
}

// An entry as the board queues it. Every byte carries `tag`, except the magic, and the sequence
// and CRC that the log stamps.
CvDiagEntry makeEntry(uint8_t tag) {
  CvDiagEntry e;
  std::memset(&e, tag, sizeof e);
  e.magic = CV_DIAG_MAGIC;
  e.sequence = 0;
  e.crc32 = 0;
  return e;
}

const uint8_t* rawOf(const CvDiagEntry& e) { return reinterpret_cast<const uint8_t*>(&e); }

void queueEntry(EntryLog& entries, const CvDiagEntry& e) { entries.append(rawOf(e)); }

const CvDiagEntry& slotAt(const FakeFlash& f, uint32_t slot) {
  return *reinterpret_cast<const CvDiagEntry*>(f.mem[kLogSector].data() + slot * CV_DIAG_ENTRY_SIZE);
}

// The bootloader's test of a slot: the magic matches and the trailer is the CRC of the rest.
bool slotValid(const FakeFlash& f, uint32_t slot) {
  const CvDiagEntry& e = slotAt(f, slot);
  return e.magic == CV_DIAG_MAGIC && e.crc32 == crc32(rawOf(e), offsetof(CvDiagEntry, crc32));
}

bool slotBlank(const FakeFlash& f, uint32_t slot) {
  return isErased(f.mem[kLogSector].data() + slot * CV_DIAG_ENTRY_SIZE, CV_DIAG_ENTRY_SIZE);
}

// Writes a valid entry straight into a slot, for scenarios that start from a used sector.
void plantEntry(FakeFlash& f, uint32_t slot, uint32_t sequence, uint8_t tag = 0x11) {
  CvDiagEntry e = makeEntry(tag);
  e.sequence = sequence;
  e.crc32 = crc32(rawOf(e), offsetof(CvDiagEntry, crc32));
  std::memcpy(f.mem[kLogSector].data() + slot * CV_DIAG_ENTRY_SIZE, &e, sizeof e);
}

void fillSector(FakeFlash& f, uint32_t firstSequence) {
  for (uint32_t slot = 0; slot < kSlots; ++slot) plantEntry(f, slot, firstSequence + slot);
}

// Polls until the log is idle and returns the most flash operations one poll started.
int runToIdle(EntryLog& entries, FakeFlash& f) {
  int worst = 0;
  for (int i = 0; i < 2000 && entries.busy(); ++i) {
    const int before = f.ops;
    entries.poll();
    worst = std::max(worst, f.ops - before);
  }
  return worst;
}

void checkEmptySector() {
  FakeFlash f;
  EntryLog entries = makeLog(f);
  check(entries.begin(), "entry log: the contract geometry opens");
  check(entries.count() == 0 && entries.lastSequence() == 0 && !entries.busy() && !entries.takeError(),
        "entry log: an erased sector is an empty log");
  entries.poll();
  check(f.ops == 0, "entry log: an empty queue starts no flash operation");
}

void checkAppendAndScanBack() {
  FakeFlash f;
  EntryLog entries = makeLog(f);
  entries.begin();
  int worst = 0;
  for (uint8_t i = 0; i < 3; ++i) {
    queueEntry(entries, makeEntry(static_cast<uint8_t>(0x20 + i)));
    worst = std::max(worst, runToIdle(entries, f));
  }
  check(f.programs == 3 * kWordsPerEntry && f.erases[kLogSector] == 0,
        "entry log: an entry is eight word programs and a blank sector needs no erase");
  check(worst == 1, "entry log: at most one flash operation per poll");
  check(entries.count() == 3 && entries.lastSequence() == 3, "entry log: the counts follow the writes");

  bool inOrder = true;
  for (uint32_t slot = 0; slot < 3; ++slot) {
    inOrder = inOrder && slotValid(f, slot) && slotAt(f, slot).sequence == slot + 1 &&
              slotAt(f, slot).summary[10] == static_cast<char>(0x20 + slot);
  }
  check(inOrder, "entry log: entries land in order with a sequence, a valid CRC and the caller's bytes");
  check(slotBlank(f, 3), "entry log: the slot after the last entry stays blank");

  EntryLog again = makeLog(f);
  check(again.begin() && again.count() == 3 && again.lastSequence() == 3, "entry log: a rescan finds what was written");
  queueEntry(again, makeEntry(0x30));
  runToIdle(again, f);
  check(slotValid(f, 3) && slotAt(f, 3).sequence == 4, "entry log: after a rescan the next entry continues the sequence");
}

void checkTornEntry() {
  FakeFlash f;
  for (uint32_t slot = 0; slot < 3; ++slot) plantEntry(f, slot, slot + 1);
  std::memset(f.mem[kLogSector].data() + 2 * CV_DIAG_ENTRY_SIZE + 3 * kFlashWordBytes, 0xFF,
              (kWordsPerEntry - 3) * kFlashWordBytes);  // slot 2 cut off after three words
  EntryLog entries = makeLog(f);
  entries.begin();
  check(entries.count() == 2 && entries.lastSequence() == 2, "entry log: a torn entry is skipped");
  queueEntry(entries, makeEntry(0x40));
  runToIdle(entries, f);
  check(!slotValid(f, 2) && !slotBlank(f, 2), "entry log: the torn slot is left as it was");
  check(slotValid(f, 3) && slotAt(f, 3).sequence == 3, "entry log: the next entry goes after the torn slot");
}

void checkCrcMismatch() {
  FakeFlash newest;
  for (uint32_t slot = 0; slot < 3; ++slot) plantEntry(newest, slot, slot + 1);
  newest.mem[kLogSector][2 * CV_DIAG_ENTRY_SIZE + 100] ^= 0x01;
  EntryLog a = makeLog(newest);
  a.begin();
  check(a.count() == 2 && a.lastSequence() == 2, "entry log: a damaged newest entry does not set the sequence");

  FakeFlash middle;
  for (uint32_t slot = 0; slot < 3; ++slot) plantEntry(middle, slot, slot + 1);
  middle.mem[kLogSector][1 * CV_DIAG_ENTRY_SIZE + 7] ^= 0x80;
  EntryLog b = makeLog(middle);
  b.begin();
  check(b.count() == 2 && b.lastSequence() == 3, "entry log: a damaged entry is skipped and the later ones still count");
}

void checkEraseWhenFull() {
  FakeFlash f;
  fillSector(f, 100);
  EntryLog entries = makeLog(f);
  entries.begin();
  check(entries.count() == kSlots && entries.lastSequence() == 100 + kSlots - 1, "entry log: a full sector scans in full");
  queueEntry(entries, makeEntry(0x50));
  const int worst = runToIdle(entries, f);
  check(f.erases[kLogSector] == 1 && worst == 1, "entry log: a full sector is erased first, one operation per poll");
  check(slotValid(f, 0) && slotAt(f, 0).sequence == 100 + kSlots && slotBlank(f, 1),
        "entry log: the log restarts at slot 0 and the sequence keeps counting");
  check(entries.count() == 1 && entries.lastSequence() == 100 + kSlots, "entry log: the counts restart with the erase");
}

void checkPendingReplacement() {
  FakeFlash f;
  EntryLog entries = makeLog(f);
  entries.begin();
  queueEntry(entries, makeEntry(0x61));
  queueEntry(entries, makeEntry(0x62));
  check(entries.dropped() == 1, "entry log: a second entry before the first starts replaces it and counts a drop");
  runToIdle(entries, f);
  check(entries.count() == 1 && slotAt(f, 0).summary[10] == 0x62 && slotBlank(f, 1),
        "entry log: only the replacing entry is written");

  queueEntry(entries, makeEntry(0x63));
  entries.poll();  // the write starts and its bytes are fixed
  queueEntry(entries, makeEntry(0x64));
  check(entries.dropped() == 1, "entry log: an entry behind a running write queues without a drop");
  runToIdle(entries, f);
  check(slotAt(f, 1).summary[10] == 0x63 && slotAt(f, 2).summary[10] == 0x64 && slotAt(f, 2).sequence == 3,
        "entry log: the running write keeps its bytes and the queued entry follows");
}

void checkErrors() {
  FakeFlash f;
  EntryLog entries = makeLog(f);
  entries.begin();
  f.failNextProgram = true;
  queueEntry(entries, makeEntry(0x81));
  runToIdle(entries, f);
  check(entries.takeError() && !entries.takeError(), "entry log: a failed program is reported once");
  check(!entries.busy() && entries.count() == 0 && entries.lastSequence() == 0,
        "entry log: a failed program drops the entry and uses no sequence");
  queueEntry(entries, makeEntry(0x82));
  runToIdle(entries, f);
  check(!entries.takeError() && slotValid(f, 1) && slotAt(f, 1).sequence == 1 && entries.count() == 1,
        "entry log: the next entry skips the failed slot");

  FakeFlash full;
  fillSector(full, 1);
  EntryLog stuck = makeLog(full);
  stuck.begin();
  full.failNextErase = true;
  queueEntry(stuck, makeEntry(0x83));
  runToIdle(stuck, full);
  check(stuck.takeError() && stuck.count() == kSlots && slotValid(full, 0) && slotAt(full, 0).sequence == 1,
        "entry log: a failed erase is reported and leaves the entries alone");
  queueEntry(stuck, makeEntry(0x84));
  runToIdle(stuck, full);
  check(full.erases[kLogSector] == 2 && slotAt(full, 0).sequence == kSlots + 1 && !stuck.takeError(),
        "entry log: the next entry erases again and keeps the sequence the failed one did not use");
}

void checkDirtySector() {
  FakeFlash f;
  plantEntry(f, 3, 7);  // an erase was cut off: blank slots, then old data
  plantEntry(f, 4, 8);
  EntryLog entries = makeLog(f);
  entries.begin();
  check(entries.count() == 2 && entries.lastSequence() == 8, "entry log: a dirty sector still reports its entries");
  queueEntry(entries, makeEntry(0x91));
  runToIdle(entries, f);
  check(f.erases[kLogSector] == 1 && slotValid(f, 0) && slotAt(f, 0).sequence == 9 && slotBlank(f, 3),
        "entry log: data after a blank slot is erased before the next entry");
}

void checkGate() {
  FakeFlash f;
  EntryLog entries = makeLog(f, gate);
  entries.begin();
  gGateOpen = false;
  queueEntry(entries, makeEntry(0xA1));
  for (int i = 0; i < 20; ++i) entries.poll();
  check(f.ops == 0 && entries.busy(), "entry log: a closed gate starts nothing and keeps the entry");
  gGateOpen = true;
  runToIdle(entries, f);
  check(slotValid(f, 0) && !entries.busy(), "entry log: the entry is written once the gate opens");
}

void checkGeometry() {
  FakeFlash f;
  const CvDiagEntry e = makeEntry(0xB1);
  EntryLog notWords(f, kLogSector, 100, 4);
  EntryLog tooBig(f, kLogSector, 512, 4);
  EntryLog overlap(f, kLogSector, CV_DIAG_ENTRY_SIZE, CV_DIAG_ENTRY_SIZE - 4);
  EntryLog empty(f, kLogSector, 0, 0);
  check(!notWords.begin() && !tooBig.begin() && !overlap.begin() && !empty.begin(),
        "entry log: geometry the flash or the entry cannot hold is refused");
  queueEntry(notWords, e);
  notWords.poll();
  check(notWords.dropped() == 1 && !notWords.busy() && f.ops == 0, "entry log: a closed log refuses entries");
}

}  // namespace

void checkEntryLog() {
  checkEmptySector();
  checkAppendAndScanBack();
  checkTornEntry();
  checkCrcMismatch();
  checkEraseWhenFull();
  checkPendingReplacement();
  checkErrors();
  checkDirtySector();
  checkGate();
  checkGeometry();
}
