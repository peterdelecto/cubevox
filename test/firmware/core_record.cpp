// Checks the layout record codec and the flash record store on the host. Covers encoding,
// recovery after torn writes and interrupted erases, sector switches and flash errors.

#include "support.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/layout.h"
#include "core/record.h"
#include "core/record_store.h"
#include "core/registry.h"
#include "core/settings.h"

namespace {

// In-memory bank 2: two 128 KB sectors, 0xFF erased. Programming ANDs bits in like real flash.
// failNextErase and failNextProgram make the next such operation report an error.
class FakeFlash : public FlashIo {
 public:
  FakeFlash() {
    mem[0].assign(kRecordSectorBytes, 0xFF);
    mem[1].assign(kRecordSectorBytes, 0xFF);
  }
  const uint8_t* read(int s) const override { return mem[s].data(); }
  void eraseStart(int s) override {
    erasing = s;
    busyPolls = 3;
    ++erases[s];
    ++ops;
  }
  bool eraseBusy() override {
    if (erasing < 0) return false;
    if (busyPolls-- > 0) return true;
    if (failNextErase) {
      error = true;
      failNextErase = false;
    } else {
      mem[erasing].assign(kRecordSectorBytes, 0xFF);
    }
    erasing = -1;
    return false;
  }
  void programWord(int s, uint32_t off, const uint8_t* w) override {
    ++programs;
    ++ops;
    if (failNextProgram) {
      error = true;
      failNextProgram = false;
      return;
    }
    for (size_t i = 0; i < kFlashWordBytes; ++i) mem[s][off + i] &= w[i];
  }
  bool takeError() override {
    const bool e = error;
    error = false;
    return e;
  }

  void put(int sector, int entry, const RecordData& rec) {
    const RecordBytes b = encodeRecord(rec);
    std::memcpy(&mem[sector][static_cast<size_t>(entry) * kRecordBytes], b.data(), kRecordBytes);
  }

  std::vector<uint8_t> mem[2];
  int erasing = -1;
  int busyPolls = 0;
  int erases[2] = {0, 0};
  int programs = 0;
  int ops = 0;
  bool error = false;
  bool failNextErase = false;
  bool failNextProgram = false;
};

RecordData makeRec(uint32_t seq, OutputLevel out = OutputLevel::Line, const char* name = "") {
  RecordData r;
  r.sequence = seq;
  r.settings.outputLevel = out;
  std::strncpy(r.name, name, kLayoutNameBytes);
  return r;
}

// Polls until the store is idle; returns the largest number of flash ops one poll started.
int runToIdle(RecordStore& store, FakeFlash& f, uint32_t& t) {
  int worst = 0;
  for (int i = 0; i < 100 && store.status().busy; ++i) {
    const int before = f.ops;
    store.poll(t++);
    worst = std::max(worst, f.ops - before);
  }
  return worst;
}

void checkRecordCodec() {
  const uint8_t vec[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  check(crc32(vec, sizeof vec) == 0xCBF43926u, "record: crc32 of 123456789");

  RecordData in = makeRec(77, OutputLevel::Instrument, "stage rig");
  in.settings.inputJack = InputJack::Pedal;
  in.layout.card[2] = Card::ReverbChasm;
  in.layout.card[7] = Card::Empty;
  RecordBytes b = encodeRecord(in);
  RecordData out;
  check(decodeRecord(b.data(), out), "record: round trip decodes");
  check(out.sequence == 77 && out.settings.outputLevel == OutputLevel::Instrument &&
            out.settings.inputJack == InputJack::Pedal && std::strcmp(out.name, "stage rig") == 0 &&
            out.layout.card == in.layout.card && !out.unknownCard,
        "record: round trip keeps every field");

  RecordBytes bad = b;
  bad[kOffName] ^= 0x01;
  check(!decodeRecord(bad.data(), out), "record: corrupt byte fails the CRC");
  bad = b;
  bad[kOffSchema] = 2;
  check(!decodeRecord(bad.data(), out), "record: wrong schema rejected");
}

void checkRecordUnknownCard() {
  const RecordBytes good = encodeRecord(makeRec(1));
  RecordBytes bad = good;
  std::memset(&bad[kOffSlots], 0, kCardIdBytes);
  std::memcpy(&bad[kOffSlots], "no_such_card", 12);
  const uint32_t crc = crc32(bad.data(), kOffCrc);
  for (int i = 0; i < 4; ++i) bad[kOffCrc + i] = static_cast<uint8_t>(crc >> (8 * i));
  RecordData out;
  check(decodeRecord(bad.data(), out) && out.layout.card[0] == Card::Empty, "record: unknown card id becomes empty");
  check(out.unknownCard, "record: an unknown card id is flagged");
  check(decodeRecord(good.data(), out) && !out.unknownCard, "record: known ids raise no flag");
}

void checkRecordEntryStates() {
  const RecordBytes b = encodeRecord(makeRec(1));
  const uint8_t blank[kRecordBytes] = {};
  std::vector<uint8_t> ff(kRecordBytes, 0xFF);
  check(classifyRecordEntry(ff.data()) == EntryState::Blank, "record: all 0xFF is blank");
  check(classifyRecordEntry(blank) == EntryState::Torn, "record: zeros are torn");
  check(classifyRecordEntry(b.data()) == EntryState::Valid, "record: an encoded entry is valid");
  check(kRecordCardFieldBytes == kCardIdBytes + kCardFieldReservedBytes && kRecordBytes % kFlashWordBytes == 0,
        "record: field widths derive from the id width and the flash word");
}

void checkRecordNewest() {
  FakeFlash f;
  f.put(0, 0, makeRec(3));
  f.put(1, 0, makeRec(5, OutputLevel::Instrument));
  f.put(0, 1, makeRec(4));
  const uint8_t* views[2] = {f.read(0), f.read(1)};
  RecordFound found;
  check(newestRecord(views, 2, kRecordSectorBytes, found) && found.rec.sequence == 5 && found.sector == 1,
        "record: newest wins across sectors");

  RecordStore store(f);
  RecordData out;
  check(store.begin(out) && out.sequence == 5, "store: boot loads the newest");
  check(store.status().sector == 1 && store.status().nextEntry == 1, "store: append goes after the newest");
}

void checkStoreAppend() {
  FakeFlash f;
  RecordStore store(f);
  RecordData out;
  check(!store.begin(out), "store: blank flash has no record");

  Settings s;
  s.outputLevel = OutputLevel::Instrument;
  store.request(defaultLayout(), s, 100);
  store.poll(2099);
  check(f.ops == 0, "store: no write before 2 s of quiet");
  store.request(defaultLayout(), s, 1500);
  store.poll(3499);
  check(f.ops == 0, "store: a new request restarts the debounce");
  uint32_t t = 3500;
  const int worst = runToIdle(store, f, t);
  check(f.programs == kWordsPerRecord && f.erases[0] + f.erases[1] == 0, "store: one record is eight word programs, no erase");
  check(worst == 1, "store: at most one flash op per poll");

  FakeFlash again;
  std::memcpy(again.mem[0].data(), f.mem[0].data(), kRecordBytes);
  RecordStore reboot(again);
  check(reboot.begin(out) && out.sequence == 1 && out.settings.outputLevel == OutputLevel::Instrument,
        "store: the saved record reloads");

  store.saveNow();
  store.poll(3501 + 1000);
  check(f.programs == kWordsPerRecord, "store: save-now starts without waiting (first poll sets up)");
  runToIdle(store, f, t);
  check(f.programs == 2 * kWordsPerRecord && store.status().sequence == 2 && store.status().nextEntry == 2,
        "store: second record appends after the first");
}

// A request stamped later than the poll's clock (two clock reads in one pass) is not yet due.
void checkStoreClockSkew() {
  FakeFlash f;
  RecordStore store(f);
  RecordData out;
  store.begin(out);
  store.request(defaultLayout(), Settings{}, 5000);
  store.poll(4000);
  store.poll(4001);
  check(f.ops == 0, "store: a poll earlier than the request does not save");
  store.poll(6999);
  check(f.ops == 0, "store: still waiting just under the debounce");
  uint32_t t = 7000;
  runToIdle(store, f, t);
  check(f.programs == kWordsPerRecord, "store: the save runs once the debounce has passed");
}

void checkStoreTornTail() {
  FakeFlash f;
  f.put(0, 0, makeRec(1));
  f.put(0, 1, makeRec(2, OutputLevel::Instrument));
  f.put(0, 2, makeRec(3));
  std::memset(&f.mem[0][2 * kRecordBytes + 3 * kFlashWordBytes], 0xFF, 5 * kFlashWordBytes);  // cut after 3 words
  f.mem[0][2 * kRecordBytes + 100] = 0x00;                                                        // and damaged
  RecordStore store(f);
  RecordData out;
  check(store.begin(out) && out.sequence == 2, "store: torn tail is skipped, previous record loads");
  check(store.status().nextEntry == 3, "store: append goes after the torn entry");
  check(store.bootNotes().tornEntries == 1 && store.bootNotes().dirtySectors == 0 && !store.bootNotes().unknownCard,
        "store: boot notes count the torn entry");
  store.request(defaultLayout(), Settings{}, 0);
  uint32_t t = kSaveDebounceMs;
  runToIdle(store, f, t);
  const uint8_t* views[2] = {f.read(0), f.read(1)};
  RecordFound found;
  check(newestRecord(views, 2, kRecordSectorBytes, found) && found.rec.sequence == 3 && found.entry == 3,
        "store: record lands in entry 3 with sequence 3");
}

void fillSector(FakeFlash& f, int sector, uint32_t firstSeq) {
  for (int entry = 0; entry < kEntriesPerSector; ++entry) f.put(sector, entry, makeRec(firstSeq + static_cast<uint32_t>(entry)));
}

void checkStoreSectorSwitch() {
  FakeFlash f;
  fillSector(f, 0, 1);
  f.put(1, 0, makeRec(0));
  std::memset(&f.mem[1][kRecordBytes], 0x5A, 64);  // stale junk in the other sector
  RecordStore store(f);
  RecordData out;
  check(store.begin(out) && out.sequence == kEntriesPerSector, "store: full sector loads its last record");
  store.request(defaultLayout(), Settings{}, 0);
  uint32_t t = kSaveDebounceMs;
  const int worst = runToIdle(store, f, t);
  check(f.erases[1] == 1 && f.erases[0] == 0, "store: full sector erases the other one");
  check(worst == 1, "store: erase start and programs are separate polls");
  check(store.status().sector == 1 && store.status().nextEntry == 1 && store.status().sequence == kEntriesPerSector + 1,
        "store: new record is entry 0 of the other sector");
  const uint8_t* views[2] = {f.read(0), f.read(1)};
  RecordFound found;
  check(newestRecord(views, 2, kRecordSectorBytes, found) && found.sector == 1 && found.entry == 0,
        "store: newest is in the fresh sector");
}

void checkStoreInterruptedErase() {
  // The older sector was mid-erase: a blank entry followed by old data.
  FakeFlash f;
  fillSector(f, 1, 1);
  std::memset(&f.mem[1][0], 0xFF, 3 * kRecordBytes);
  fillSector(f, 0, 600);
  RecordStore store(f);
  RecordData out;
  check(store.begin(out) && out.sequence == 600 + kEntriesPerSector - 1, "store: dirty sector does not hide the newest");
  check(store.bootNotes().dirtySectors == 1, "store: boot notes count the dirty sector");
  store.request(defaultLayout(), Settings{}, 0);
  uint32_t t = kSaveDebounceMs;
  runToIdle(store, f, t);
  check(f.erases[1] == 1, "store: interrupted erase is re-erased before use");

  // No valid record anywhere and sector 0 dirty: use the blank sector, leave the dirty one.
  FakeFlash g;
  std::memset(&g.mem[0][kRecordBytes], 0x33, 64);
  RecordStore fresh(g);
  check(!fresh.begin(out), "store: junk is not a record");
  fresh.request(defaultLayout(), Settings{}, 0);
  t = kSaveDebounceMs;
  runToIdle(fresh, g, t);
  check(g.erases[0] + g.erases[1] == 0 && fresh.status().sector == 1 && fresh.status().sequence == 1,
        "store: a blank sector is used without an erase");
}

void checkStoreEraseAll() {
  FakeFlash g;
  RecordStore fresh(g);
  RecordData out;
  fresh.begin(out);
  fresh.request(defaultLayout(), Settings{}, 0);
  uint32_t t = kSaveDebounceMs;
  runToIdle(fresh, g, t);
  check(fresh.eraseAll(), "store: erase-all accepted when idle");
  check(!fresh.eraseAll(), "store: erase-all is refused while one runs");
  for (int i = 0; i < 50; ++i) fresh.poll(t++);
  check(g.erases[0] == 1 && g.erases[1] == 1 && !fresh.status().hasRecord && !fresh.status().busy,
        "store: erase-all clears both sectors");
}

void checkStoreProgramFailure() {
  FakeFlash f;
  RecordStore store(f);
  RecordData out;
  store.begin(out);
  f.failNextProgram = true;
  store.request(defaultLayout(), Settings{}, 0);
  uint32_t t = kSaveDebounceMs;
  runToIdle(store, f, t);
  check(store.status().errors == 1 && store.status().sequence == 0 && !store.status().busy,
        "store: a failed program counts an error and keeps the old sequence");
  check(store.takeSaveFailed() && !store.takeSaveFailed(), "store: a failed save is reported once");
  check(store.status().nextEntry == 1, "store: the part-written entry is skipped");

  store.request(defaultLayout(), Settings{}, t);
  t += kSaveDebounceMs;
  runToIdle(store, f, t);
  check(store.status().errors == 1 && store.status().sequence == 1 && store.status().nextEntry == 2 && !store.takeSaveFailed(),
        "store: the next save lands after the skipped entry");
}

void checkStoreEraseFailure() {
  FakeFlash f;
  fillSector(f, 1, 1);     // old records in the sector the log has to move to
  fillSector(f, 0, 1000);  // the full, newest sector
  const uint32_t newest = 1000 + kEntriesPerSector - 1;
  RecordStore store(f);
  RecordData out;
  store.begin(out);
  f.failNextErase = true;
  store.request(defaultLayout(), Settings{}, 0);
  uint32_t t = kSaveDebounceMs;
  runToIdle(store, f, t);
  check(store.status().errors == 1 && store.takeSaveFailed(), "store: a failed erase counts an error and is reported");
  check(store.status().sequence == newest, "store: a failed erase keeps the old sequence");

  store.request(defaultLayout(), Settings{}, t);
  t += kSaveDebounceMs;
  runToIdle(store, f, t);
  check(f.erases[1] == 2 && store.status().sector == 1 && store.status().sequence == newest + 1,
        "store: the sector is erased again and the save lands");

  FakeFlash g;
  RecordStore wipe(g);
  wipe.begin(out);
  g.failNextErase = true;
  wipe.eraseAll();
  t = 0;
  for (int i = 0; i < 50; ++i) wipe.poll(t++);
  check(wipe.status().errors == 1 && wipe.takeSaveFailed(), "store: a failed erase-all counts an error and is reported");
}

void checkStoreWhileBusy() {
  FakeFlash f;
  RecordStore store(f);
  RecordData out;
  store.begin(out);
  Settings first;
  first.outputLevel = OutputLevel::Instrument;
  store.request(defaultLayout(), first, 0);
  uint32_t t = kSaveDebounceMs;
  store.poll(t++);  // the write starts and its record is fixed
  check(!store.eraseAll(), "store: erase-all is refused while a save runs");
  store.request(defaultLayout(), Settings{}, t);  // a newer state arrives mid-write
  store.saveNow();
  runToIdle(store, f, t);
  RecordData back;
  check(decodeRecord(f.mem[0].data(), back) && back.settings.outputLevel == OutputLevel::Instrument,
        "store: a request during programming does not change the record being written");
  check(decodeRecord(f.mem[0].data() + kRecordBytes, back) && back.settings.outputLevel == OutputLevel::Line && back.sequence == 2,
        "store: the save-now behind it writes the newer state next");
}

void checkStoreName() {
  FakeFlash f;
  RecordStore store(f);
  RecordData out;
  store.begin(out);
  store.setName("stage rig");
  store.request(defaultLayout(), Settings{}, 0);
  uint32_t t = 2000;
  runToIdle(store, f, t);
  RecordData back;
  check(decodeRecord(f.mem[0].data(), back) && std::strcmp(back.name, "stage rig") == 0, "store: the layout name is saved");
  store.setName("a name that is longer than thirty-two bytes");
  check(std::strlen(store.current().name) == kLayoutNameBytes, "store: a long name is clipped to 32 bytes");
}

void checkStoreUnknownCard() {
  FakeFlash f;
  RecordData rec = makeRec(1);
  f.put(0, 0, rec);
  std::memset(&f.mem[0][kOffSlots], 0, kCardIdBytes);
  std::memcpy(&f.mem[0][kOffSlots], "no_such_card", 12);
  const uint32_t crc = crc32(f.mem[0].data(), kOffCrc);
  for (int i = 0; i < 4; ++i) f.mem[0][kOffCrc + static_cast<size_t>(i)] = static_cast<uint8_t>(crc >> (8 * i));
  RecordStore store(f);
  RecordData out;
  check(store.begin(out) && store.bootNotes().unknownCard, "store: boot notes flag an unknown card in the newest record");
}

void checkStoreFlushPending() {
  FakeFlash f;
  RecordStore store(f);
  RecordData out;
  store.begin(out);
  store.flushPending();
  uint32_t t = 0;
  store.poll(t++);
  check(f.programs == 0 && !store.status().busy, "store: flushing with nothing pending starts nothing");

  store.request(defaultLayout(), Settings{}, 0);
  store.poll(t++);
  check(f.programs == 0 && store.status().busy, "store: a request waits out the debounce");
  store.flushPending();
  runToIdle(store, f, t);
  check(f.programs > 0 && store.status().sequence == 1 && !store.status().busy,
        "store: flushing a pending save writes it before the debounce ends");
}

}  // namespace

void checkRecords() {
  checkRecordCodec();
  checkStoreFlushPending();
  checkRecordUnknownCard();
  checkRecordEntryStates();
  checkRecordNewest();
  checkStoreAppend();
  checkStoreClockSkew();
  checkStoreName();
  checkStoreTornTail();
  checkStoreSectorSwitch();
  checkStoreInterruptedErase();
  checkStoreEraseAll();
  checkStoreProgramFailure();
  checkStoreEraseFailure();
  checkStoreWhileBusy();
  checkStoreUnknownCard();
}
