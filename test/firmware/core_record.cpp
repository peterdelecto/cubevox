// Checks the layout record codec and the flash record store on the host. Covers encoding, the
// save debounce and the skipped save of an unchanged state, recovery after torn writes and
// interrupted erases, the boot-time ECC recovery, sector switches and flash errors.

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
#include "fake_flash.h"

namespace {

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

  RecordData in = makeRec(77, OutputLevel::Mic, "stage rig");
  in.settings.inputJack = InputJack::Line;
  in.layout.card[2] = Card::ReverbChasm;
  in.layout.card[7] = Card::Empty;
  RecordBytes b = encodeRecord(in);
  RecordData out;
  check(decodeRecord(b.data(), out), "record: round trip decodes");
  check(out.sequence == 77 && out.settings.outputLevel == OutputLevel::Mic &&
            out.settings.inputJack == InputJack::Line && std::strcmp(out.name, "stage rig") == 0 &&
            out.layout.card == in.layout.card && !out.unknownCard,
        "record: round trip keeps every field");

  RecordBytes bad = b;
  bad[kOffName] ^= 0x01;
  check(!decodeRecord(bad.data(), out), "record: corrupt byte fails the CRC");
  bad = b;
  bad[kOffSchema] = 3;
  check(!decodeRecord(bad.data(), out), "record: wrong schema rejected");
}

// Rewrites the CRC after a test edits the bytes it covers.
void reseal(RecordBytes& b) {
  const uint32_t crc = crc32(b.data(), kOffCrc);
  for (size_t i = 0; i < 4; ++i) b[kOffCrc + i] = static_cast<uint8_t>(crc >> (8 * i));
}

// What a v1 firmware wrote: schema 1, and zeros where the v2 settings bytes now sit.
RecordBytes asV1(RecordBytes b) {
  b[kOffSchema] = static_cast<uint8_t>(kRecordSchemaV1);
  b[kOffSchema + 1] = 0;
  for (size_t off : {kOffPolishComp, kOffPolishEq, kOffTopSwitch}) b[off] = 0;
  reseal(b);
  return b;
}

void checkRecordSchemaV2() {
  Settings changed;
  changed.polishComp = false;
  changed.polishEq = false;
  changed.topSwitch = TopSwitch::Bypass;
  RecordData in = makeRec(9, OutputLevel::Mic);
  in.settings = changed;
  in.settings.outputLevel = OutputLevel::Mic;
  const RecordBytes b = encodeRecord(in);
  RecordData out;
  check(b[kOffSchema] == kRecordSchema && b[kOffPolishComp] == 0 && b[kOffPolishEq] == 0 &&
            b[kOffTopSwitch] == static_cast<uint8_t>(TopSwitch::Bypass),
        "record v2: the polish bytes follow the two v1 settings bytes");
  check(decodeRecord(b.data(), out) && !out.settings.polishComp && !out.settings.polishEq &&
            out.settings.topSwitch == TopSwitch::Bypass && out.settings.outputLevel == OutputLevel::Mic,
        "record v2: round trip keeps the polish settings");

  const RecordBytes defaults = encodeRecord(makeRec(1));
  check(decodeRecord(defaults.data(), out) && out.settings.polishComp && out.settings.polishEq &&
            out.settings.topSwitch == TopSwitch::Mute,
        "record v2: default settings are comp on, EQ on, MUTE");

  RecordData wasV1 = makeRec(4, OutputLevel::Mic);
  wasV1.settings.inputJack = InputJack::Line;
  const RecordBytes v1 = asV1(encodeRecord(wasV1));
  check(decodeRecord(v1.data(), out) && out.sequence == 4 && out.settings.outputLevel == OutputLevel::Mic &&
            out.settings.inputJack == InputJack::Line && out.settings.polishComp && out.settings.polishEq &&
            out.settings.topSwitch == TopSwitch::Mute,
        "record v1: decodes with the polish defaults, not the zero bytes");
  check(classifyRecordEntry(v1.data()) == EntryState::Valid, "record v1: still a valid entry");
  check(encodeRecord(out)[kOffSchema] == kRecordSchema, "record v1: the next save writes v2");

  RecordBytes junk = v1;  // a v1 reserved byte is not a setting, whatever it holds
  junk[kOffPolishComp] = 0x7F;
  junk[kOffTopSwitch] = 0xFF;
  reseal(junk);
  check(decodeRecord(junk.data(), out) && out.settings.polishComp && out.settings.topSwitch == TopSwitch::Mute,
        "record v1: bytes past the two settings are ignored");

  for (size_t off : {kOffOutputLevel, kOffInputJack, kOffPolishComp, kOffPolishEq, kOffTopSwitch, kOffDebug}) {
    RecordBytes bad = b;
    bad[off] = 2;
    reseal(bad);
    check(!decodeRecord(bad.data(), out), "record v2: a settings byte out of range is rejected");
  }
}

// DEBUG sits in the first reserved settings byte, so the schema stays 2. A record that holds 0 there,
// as every record written before the byte existed does, reads as OFF.
void checkRecordDebugByte() {
  RecordData on = makeRec(3);
  on.settings.debug = true;
  const RecordBytes b = encodeRecord(on);
  RecordData out;
  check(b[kOffSchema] == kRecordSchema && b[kOffDebug] == 1 && kOffDebug == kOffTopSwitch + 1,
        "record v2: DEBUG ON is byte 1 right after the top switch, and the schema stays 2");
  check(decodeRecord(b.data(), out) && out.settings.debug, "record v2: DEBUG ON survives a save and a reload");

  const RecordBytes off = encodeRecord(makeRec(3));
  check(off[kOffDebug] == 0 && decodeRecord(off.data(), out) && !out.settings.debug,
        "record v2: DEBUG OFF is the zero byte, which is also what an older record holds");

  RecordBytes before = b;  // an older v2 record: the same bytes with the byte at 0
  before[kOffDebug] = 0;
  reseal(before);
  check(decodeRecord(before.data(), out) && !out.settings.debug && out.sequence == 3,
        "record v2: a record written before the byte existed loads with DEBUG OFF");

  RecordBytes v1 = asV1(b);  // a v1 record has no such setting, whatever sits in the byte
  v1[kOffDebug] = 1;
  reseal(v1);
  check(decodeRecord(v1.data(), out) && !out.settings.debug, "record v1: the DEBUG byte is ignored");

  check(!sameRecordContent(b.data(), off.data()), "record v2: a DEBUG change is new content, so it writes a record");
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
  f.put(1, 0, makeRec(5, OutputLevel::Mic));
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
  s.outputLevel = OutputLevel::Mic;
  check(kSaveDebounceMs == 500, "store: a save waits 500 ms after the last change");
  store.request(defaultLayout(), s, 100);
  store.poll(100 + kSaveDebounceMs - 1);
  check(f.ops == 0, "store: no write before the debounce has passed");
  const uint32_t later = 100 + kSaveDebounceMs / 2;
  store.request(defaultLayout(), s, later);
  store.poll(later + kSaveDebounceMs - 1);
  check(f.ops == 0, "store: a new request restarts the debounce");
  uint32_t t = later + kSaveDebounceMs;
  const int worst = runToIdle(store, f, t);
  check(f.programs == kWordsPerRecord && f.erases[0] + f.erases[1] == 0, "store: one record is eight word programs, no erase");
  check(worst == 1, "store: at most one flash op per poll");

  FakeFlash again;
  std::memcpy(again.mem[0].data(), f.mem[0].data(), kRecordBytes);
  RecordStore reboot(again);
  check(reboot.begin(out) && out.sequence == 1 && out.settings.outputLevel == OutputLevel::Mic,
        "store: the saved record reloads");

  store.saveNow();
  store.poll(t + 1);
  check(f.programs == kWordsPerRecord, "store: save-now starts without waiting (first poll sets up)");
  runToIdle(store, f, t);
  check(f.programs == 2 * kWordsPerRecord && store.status().sequence == 2 && store.status().nextEntry == 2,
        "store: save-now writes even an unchanged state, after the first record");
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
  store.poll(5000 + kSaveDebounceMs - 1);
  check(f.ops == 0, "store: still waiting just under the debounce");
  uint32_t t = 5000 + kSaveDebounceMs;
  runToIdle(store, f, t);
  check(f.programs == kWordsPerRecord, "store: the save runs once the debounce has passed");
}

void checkStoreTornTail() {
  FakeFlash f;
  f.put(0, 0, makeRec(1));
  f.put(0, 1, makeRec(2, OutputLevel::Mic));
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

// Settings that differ from the ones makeRec stores, so a save has something to write.
Settings micOutput() {
  Settings s;
  s.outputLevel = OutputLevel::Mic;
  return s;
}

void checkStoreSectorSwitch() {
  FakeFlash f;
  fillSector(f, 0, 1);
  f.put(1, 0, makeRec(0));
  std::memset(&f.mem[1][kRecordBytes], 0x5A, 64);  // stale junk in the other sector
  RecordStore store(f);
  RecordData out;
  check(store.begin(out) && out.sequence == kEntriesPerSector, "store: full sector loads its last record");
  store.request(defaultLayout(), micOutput(), 0);
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
  store.request(defaultLayout(), micOutput(), 0);
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
  store.request(defaultLayout(), micOutput(), 0);
  uint32_t t = kSaveDebounceMs;
  runToIdle(store, f, t);
  check(store.status().errors == 1 && store.takeSaveFailed(), "store: a failed erase counts an error and is reported");
  check(store.status().sequence == newest, "store: a failed erase keeps the old sequence");

  store.request(defaultLayout(), micOutput(), t);
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
  first.outputLevel = OutputLevel::Mic;
  store.request(defaultLayout(), first, 0);
  uint32_t t = kSaveDebounceMs;
  store.poll(t++);  // the write starts and its record is fixed
  check(!store.eraseAll(), "store: erase-all is refused while a save runs");
  store.request(defaultLayout(), Settings{}, t);  // a newer state arrives mid-write
  store.saveNow();
  runToIdle(store, f, t);
  RecordData back;
  check(decodeRecord(f.mem[0].data(), back) && back.settings.outputLevel == OutputLevel::Mic,
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

void checkSameContent() {
  RecordData a = makeRec(7, OutputLevel::Mic, "rig");
  RecordData b = a;
  b.sequence = 8;
  const RecordBytes ea = encodeRecord(a);
  const RecordBytes eb = encodeRecord(b);
  check(ea != eb && sameRecordContent(ea.data(), eb.data()), "record: sequence and CRC aside, the same state is the same content");
  b.settings.outputLevel = OutputLevel::Line;
  const RecordBytes ec = encodeRecord(b);
  check(!sameRecordContent(ea.data(), ec.data()), "record: a changed setting is new content");
}

void checkStoreSkipsSameState() {
  FakeFlash f;
  RecordStore store(f);
  RecordData out;
  store.begin(out);
  store.request(defaultLayout(), micOutput(), 0);
  uint32_t t = kSaveDebounceMs;
  runToIdle(store, f, t);
  const int written = f.programs;
  store.request(defaultLayout(), micOutput(), t);
  t += kSaveDebounceMs;
  runToIdle(store, f, t);
  check(f.programs == written && store.status().sequence == 1 && !store.status().busy,
        "store: a save of the state flash already holds writes nothing");

  Settings line = micOutput();
  line.outputLevel = OutputLevel::Line;
  store.request(defaultLayout(), line, t);  // MIC to LINE and back inside one debounce
  store.request(defaultLayout(), micOutput(), t + 1);
  t += 1 + kSaveDebounceMs;
  runToIdle(store, f, t);
  check(f.programs == written, "store: a setting flipped and flipped back inside the debounce writes nothing");

  store.setName("stage rig");
  store.request(defaultLayout(), micOutput(), t);
  t += kSaveDebounceMs;
  runToIdle(store, f, t);
  check(f.programs == written + kWordsPerRecord && store.status().sequence == 2, "store: a new name is a change and saves");
}

void checkStoreRewritesV1() {
  FakeFlash f;
  const RecordBytes v1 = asV1(encodeRecord(makeRec(4)));
  std::memcpy(f.mem[0].data(), v1.data(), kRecordBytes);
  RecordStore store(f);
  RecordData out;
  check(store.begin(out) && out.sequence == 4, "store: a v1 record loads");
  store.request(out.layout, out.settings, 0);
  uint32_t t = kSaveDebounceMs;
  runToIdle(store, f, t);
  RecordData back;
  check(store.status().sequence == 5 && decodeRecord(f.mem[0].data() + kRecordBytes, back) &&
            f.mem[0][kRecordBytes + kOffSchema] == kRecordSchema,
        "store: the same state over a v1 record is written again as v2");
}

// Records firstSeq onward in entries 0 to records - 1 of `sector`, the next record torn at its
// fourth word, and a valid record past the tear that only a read beyond the faulted word could
// find. Returns the offset of the faulted word.
uint32_t tornTail(FakeFlash& f, int sector, uint32_t firstSeq, int records) {
  for (int i = 0; i < records; ++i) f.put(sector, i, makeRec(firstSeq + static_cast<uint32_t>(i), OutputLevel::Mic));
  const uint32_t next = firstSeq + static_cast<uint32_t>(records);
  f.put(sector, records, makeRec(next));
  const uint32_t fault = f.tear(sector, records, 3);
  f.put(sector, records + 1, makeRec(next + 50));
  return fault;
}

void checkRecoverTornTail() {
  FakeFlash f;
  fillSector(f, 1, 1);  // the older sector, full
  const uint32_t fault = tornTail(f, 0, 1000, 37);
  const uint32_t newest = 1000 + 36;
  RecordStore store(f);
  const RecordRecovery r = store.recover(0, fault);
  check(r.carried && !r.carryFailed && !r.eraseFailed && r.sequence == newest,
        "recover: the newest record below the faulted word is carried, never one past it");
  check(f.erases[1] == 1 && f.erases[0] == 1, "recover: the other sector is erased for the copy, then the faulted one");
  RecordData out;
  check(store.begin(out) && out.sequence == newest && out.settings.outputLevel == OutputLevel::Mic,
        "recover: a torn tail after 37 records boots on record 37, never the defaults");
  check(store.status().sector == 1 && store.status().nextEntry == 1, "recover: the next save appends after the copy");
}

void checkRecoverNothingBelow() {
  FakeFlash f;
  fillSector(f, 0, 1);  // the full sector the log moved away from
  const uint32_t fault = f.tear(1, 0, 2);  // the first record of the fresh sector, cut off
  RecordStore store(f);
  const RecordRecovery r = store.recover(1, fault);
  check(!r.carried && !r.carryFailed && !r.eraseFailed && f.erases[1] == 1 && f.erases[0] == 0,
        "recover: with nothing readable below the fault the sector is only erased");
  check(f.reads[0] == 0, "recover: the other sector is not read when nothing needs carrying");
  RecordData out;
  check(store.begin(out) && out.sequence == kEntriesPerSector, "recover: the box boots on the other sector's newest");
}

void checkRecoverOtherNewer() {
  FakeFlash f;
  fillSector(f, 0, 2000);                                     // the newest records
  for (int i = 0; i < 5; ++i) f.put(1, i, makeRec(10 + static_cast<uint32_t>(i)));  // left by a cut erase
  const uint32_t fault = f.tear(1, 5, 0);
  RecordStore store(f);
  const RecordRecovery r = store.recover(1, fault);
  check(!r.carried && !r.eraseFailed && f.erases[0] == 0 && f.erases[1] == 1,
        "recover: an older record below the fault is not carried over newer ones");
  RecordData out;
  check(store.begin(out) && out.sequence == 2000 + kEntriesPerSector - 1, "recover: the newest record survives");
}

void checkRecoverEraseFails() {
  FakeFlash f;
  const uint32_t fault = tornTail(f, 0, 1, 5);  // sector 1 is blank, so the copy needs no erase
  RecordStore store(f);
  f.failNextErase = true;  // the erase of the faulted sector
  const RecordRecovery r = store.recover(0, fault);
  check(r.carried && r.eraseFailed && f.erases[1] == 0, "recover: the copy lands and the failed erase is reported");
  const int readsBefore = f.reads[0];
  RecordData out;
  check(store.begin(out) && out.sequence == 5 && f.reads[0] == readsBefore,
        "recover: begin leaves the sector that would not erase unread and boots on the copy");
  check(store.bootNotes().dirtySectors == 0, "recover: an unread sector is not reported as a cut erase");
}

void checkRecoverCopyFails() {
  FakeFlash f;
  fillSector(f, 1, 1);
  const uint32_t fault = tornTail(f, 0, 1000, 20);
  RecordStore store(f);
  f.failNextErase = true;  // the erase that makes room for the copy
  const RecordRecovery r = store.recover(0, fault);
  check(r.carryFailed && !r.carried && r.sequence == 1019 && f.erases[0] == 0, "recover: a failed copy keeps the faulted sector");
  RecordData out;
  check(store.begin(out) && out.sequence == 1019, "recover: begin reads the kept sector up to the fault and boots on its newest");
  store.request(defaultLayout(), Settings{}, 0);
  uint32_t t = kSaveDebounceMs;
  runToIdle(store, f, t);
  check(f.erases[1] == 2 && f.erases[0] == 0 && store.status().sector == 1 && store.status().sequence == 1020,
        "recover: the next save erases the other sector and lands there");
}

}  // namespace

void checkRecords() {
  checkRecordCodec();
  checkRecordSchemaV2();
  checkRecordDebugByte();
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
  checkSameContent();
  checkStoreSkipsSameState();
  checkStoreRewritesV1();
  checkRecoverTornTail();
  checkRecoverNothingBelow();
  checkRecoverOtherNewer();
  checkRecoverEraseFails();
  checkRecoverCopyFails();
}
