// Checks the layout record codec and the flash record store on the host. Covers encoding, the
// save debounce and the skipped save of an unchanged state, recovery after torn writes and
// interrupted erases, the boot-time ECC recovery and its read limits, sector switches, flash errors
// and the retry, the sequence floor, and the boot load.

#include "support.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/crc32.h"
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

  RecordData growl = makeRec(78, OutputLevel::Line, "growl");
  growl.layout.card[5] = Card::Growl;
  RecordData growlOut;
  check(decodeRecord(encodeRecord(growl).data(), growlOut) && growlOut.layout.card[5] == Card::Growl && !growlOut.unknownCard,
        "record: a layout with Growl in slot 6 round trips under the card's id");

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
  for (size_t off : {kOffRetiredComp, kOffRetiredPolishEq, kOffTopSwitch, kOffCleanupEq}) b[off] = 0;
  reseal(b);
  return b;
}

void checkRecordSchemaV2() {
  Settings changed;
  changed.cleanupEq = CleanupEq::Strong;
  changed.topSwitch = TopSwitch::Bypass;
  RecordData in = makeRec(9, OutputLevel::Mic);
  in.settings = changed;
  in.settings.outputLevel = OutputLevel::Mic;
  const RecordBytes b = encodeRecord(in);
  RecordData out;
  check(b[kOffSchema] == kRecordSchema && b[kOffRetiredComp] == 0 && b[kOffRetiredPolishEq] == 0 &&
            b[kOffCleanupEq] == static_cast<uint8_t>(CleanupEq::Strong) && b[kOffTopSwitch] == static_cast<uint8_t>(TopSwitch::Bypass),
        "record v2: bytes 2 and 3 are written 0, the top switch sits at byte 4 and CLEANUP EQ at byte 9");
  check(decodeRecord(b.data(), out) && out.settings.cleanupEq == CleanupEq::Strong &&
            out.settings.topSwitch == TopSwitch::Bypass && out.settings.outputLevel == OutputLevel::Mic,
        "record v2: round trip keeps CLEANUP EQ and the top switch");

  // A record saved with byte 2 at 0, 1 or any other value loads with every other setting kept, and
  // the next save writes the byte as 0.
  for (const uint8_t held : {uint8_t{0}, uint8_t{1}, uint8_t{255}}) {
    RecordBytes old = b;
    old[kOffRetiredComp] = held;
    reseal(old);
    RecordData loaded;
    char what[96];
    std::snprintf(what, sizeof what, "record v2: retired byte 2 at %u is ignored and saved back as 0",
                  static_cast<unsigned>(held));
    check(decodeRecord(old.data(), loaded) && loaded.settings.cleanupEq == in.settings.cleanupEq &&
              loaded.settings.topSwitch == in.settings.topSwitch && loaded.settings.outputLevel == in.settings.outputLevel &&
              loaded.settings.inputJack == in.settings.inputJack && loaded.settings.debug == in.settings.debug &&
              loaded.settings.brightness == in.settings.brightness && loaded.layout == in.layout &&
              loaded.sequence == in.sequence && encodeRecord(loaded)[kOffRetiredComp] == 0,
          what);
  }

  // Byte 9 holds 0 for OFF, 1 for LIGHT and 2 for STRONG, so a saved setting loads as the same setting.
  struct EqByte {
    CleanupEq setting;
    uint8_t byte;
  };
  for (const EqByte e : {EqByte{CleanupEq::Off, 0}, EqByte{CleanupEq::Light, 1}, EqByte{CleanupEq::Strong, 2}}) {
    RecordData each = makeRec(3);
    each.settings.cleanupEq = e.setting;
    const RecordBytes eachBytes = encodeRecord(each);
    char what[80];
    std::snprintf(what, sizeof what, "record v2: CLEANUP EQ at byte value %u round trips", static_cast<unsigned>(e.byte));
    check(eachBytes[kOffCleanupEq] == e.byte && decodeRecord(eachBytes.data(), out) && out.settings.cleanupEq == e.setting,
          what);
  }

  const RecordBytes defaults = encodeRecord(makeRec(1));
  check(decodeRecord(defaults.data(), out) && out.settings.cleanupEq == CleanupEq::Off &&
            out.settings.topSwitch == TopSwitch::Mute,
        "record v2: default settings are CLEANUP EQ off, MUTE");

  RecordBytes oldOn = defaults;  // a record written while POLISH EQ was an ON/OFF row, stored ON
  oldOn[kOffRetiredPolishEq] = 1;
  oldOn[kOffCleanupEq] = 0;
  reseal(oldOn);
  check(decodeRecord(oldOn.data(), out) && out.settings.cleanupEq == CleanupEq::Off,
        "record v2: an old record with POLISH EQ stored ON loads with CLEANUP EQ OFF");
  RecordBytes junkRetired = encodeRecord(in);
  junkRetired[kOffRetiredPolishEq] = 0x7F;
  reseal(junkRetired);
  check(decodeRecord(junkRetired.data(), out) && out.settings.cleanupEq == CleanupEq::Strong,
        "record v2: the retired POLISH EQ byte is ignored, whatever it holds");

  RecordData wasV1 = makeRec(4, OutputLevel::Mic);
  wasV1.settings.inputJack = InputJack::Line;
  const RecordBytes v1 = asV1(encodeRecord(wasV1));
  check(decodeRecord(v1.data(), out) && out.sequence == 4 && out.settings.outputLevel == OutputLevel::Mic &&
            out.settings.inputJack == InputJack::Line && out.settings.cleanupEq == CleanupEq::Off &&
            out.settings.topSwitch == TopSwitch::Mute,
        "record v1: decodes with the later settings at their defaults");
  check(classifyRecordEntry(v1.data()) == EntryState::Valid, "record v1: still a valid entry");
  check(encodeRecord(out)[kOffSchema] == kRecordSchema, "record v1: the next save writes v2");

  RecordBytes junk = v1;  // a v1 reserved byte is not a setting, whatever it holds
  junk[kOffCleanupEq] = 0x7F;
  junk[kOffTopSwitch] = 0xFF;
  reseal(junk);
  check(decodeRecord(junk.data(), out) && out.settings.cleanupEq == CleanupEq::Off && out.settings.topSwitch == TopSwitch::Mute,
        "record v1: bytes past the two settings are ignored");

  for (size_t off : {kOffOutputLevel, kOffInputJack, kOffTopSwitch, kOffDebug}) {
    RecordBytes bad = b;
    bad[off] = 2;
    reseal(bad);
    check(!decodeRecord(bad.data(), out), "record v2: a settings byte out of range is rejected");
  }
  RecordBytes badEq = b;
  badEq[kOffCleanupEq] = 3;
  reseal(badEq);
  check(!decodeRecord(badEq.data(), out), "record v2: a CLEANUP EQ byte past STRONG is rejected");
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

// The brightness sits in settings byte 8, the first reserved byte after the retired pair, so the schema
// stays 2. It holds the level's index plus one: 1 LOW, 2 MID, 3 HIGH, 4 MAX. A record that holds 0
// there, as every record written before the byte existed does, reads as HIGH.
void checkRecordBrightnessByte() {
  check(kOffBrightness == kOffSettings + 8, "record: brightness is settings byte 8");

  RecordData out;
  for (size_t i = 0; i < kBrightnessLevels.size(); ++i) {
    RecordData in = makeRec(5);
    in.settings.brightness = static_cast<Brightness>(i);
    const RecordBytes b = encodeRecord(in);
    check(b[kOffSchema] == kRecordSchema && b[kOffBrightness] == i + 1,
          "record: brightness is written as the level plus one, and the schema stays 2");
    check(decodeRecord(b.data(), out) && out.settings.brightness == in.settings.brightness && out.sequence == 5,
          "record: each brightness level survives a save and a reload");
  }

  const RecordBytes high = encodeRecord(makeRec(5));
  check(high[kOffBrightness] == 3 && decodeRecord(high.data(), out) && out.settings.brightness == Brightness::High,
        "record: the default settings save HIGH, byte 3");

  RecordBytes before = high;  // a record from before the byte existed: the same bytes with 0 there
  before[kOffBrightness] = 0;
  reseal(before);
  check(decodeRecord(before.data(), out) && out.settings.brightness == Brightness::High && out.sequence == 5,
        "record: a record with byte 8 at 0 reads HIGH");
  check(encodeRecord(out)[kOffBrightness] == 3, "record: and the next save writes HIGH as byte 3");

  for (const uint8_t byte : {uint8_t{5}, uint8_t{0x7F}, uint8_t{0xFF}}) {
    RecordBytes bad = high;
    bad[kOffBrightness] = byte;
    reseal(bad);
    check(!decodeRecord(bad.data(), out), "record: a brightness byte above 4 makes the record invalid");
    check(classifyRecordEntry(bad.data()) == EntryState::Torn, "record: and the entry reads as torn");
  }

  RecordData low = makeRec(5);
  low.settings.brightness = Brightness::Low;
  const RecordBytes lowBytes = encodeRecord(low);
  const RecordBytes v1 = asV1(lowBytes);  // a v1 record has no such setting, whatever sits in the byte
  check(decodeRecord(v1.data(), out) && out.settings.brightness == Brightness::High,
        "record v1: the brightness byte is ignored");
  check(!sameRecordContent(high.data(), lowBytes.data()),
        "record: a brightness change is new content, so it writes a record");
}

// Settings bytes 6 and 7 are retired, so the schema stays 2. They are written 0 and ignored on read,
// whatever they hold. A record from the firmware that kept the input cutoffs there loads here, and
// a record from this firmware reads on that firmware as its default cutoffs, because that firmware
// reads a 0 as not set.
void checkRecordRetiredBytes() {
  check(kOffRetired == kOffSettings + 6 && kRetiredBytes == 2 && kOffRetired + kRetiredBytes <= kOffCrc,
        "record: settings bytes 6 and 7 are the retired pair");

  RecordData in = makeRec(2);
  in.settings.cleanupEq = CleanupEq::Light;
  in.settings.debug = true;
  const RecordBytes saved = encodeRecord(in);
  check(saved[kOffSchema] == kRecordSchema && saved[kOffRetired] == 0 && saved[kOffRetired + 1] == 0,
        "record: bytes 6 and 7 are written 0 and the schema stays 2");

  RecordData out;
  RecordBytes earlier = saved;  // the earlier firmware wrote the cutoff list index plus one here
  earlier[kOffRetired] = 4;
  earlier[kOffRetired + 1] = 7;
  reseal(earlier);
  check(decodeRecord(earlier.data(), out) && out.settings.cleanupEq == in.settings.cleanupEq &&
            out.settings.topSwitch == in.settings.topSwitch && out.settings.debug && out.layout == in.layout &&
            out.sequence == in.sequence,
        "record: a record from the earlier firmware loads with every other setting intact");

  for (const uint8_t byte : {uint8_t{1}, uint8_t{5}, uint8_t{0x7F}, uint8_t{0xFF}}) {
    RecordBytes odd = saved;
    odd[kOffRetired] = byte;
    odd[kOffRetired + 1] = byte;
    reseal(odd);
    check(decodeRecord(odd.data(), out) && out.settings.cleanupEq == in.settings.cleanupEq && out.settings.debug,
          "record: whatever bytes 6 and 7 hold, the record stays valid");
  }

  RecordBytes v1 = asV1(saved);  // a v1 record has no such setting, whatever sits in the bytes
  v1[kOffRetired] = 1;
  v1[kOffRetired + 1] = 2;
  reseal(v1);
  check(decodeRecord(v1.data(), out), "record v1: the retired bytes are ignored");

  RecordBytes reencoded = encodeRecord(out);
  check(reencoded[kOffRetired] == 0 && reencoded[kOffRetired + 1] == 0,
        "record: a record loaded with those bytes set is written back with 0");
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
  check(store.status().nextEntry == kEntriesPerSector, "store: a sector with a torn entry takes no more appends");
  check(store.bootNotes().tornEntries == 1 && store.bootNotes().dirtySectors == 0 && !store.bootNotes().unknownCard,
        "store: boot notes count the torn entry");
  check(!store.status().busy, "store: a torn tail past the newest record starts no save");
  store.request(defaultLayout(), Settings{}, 0);
  uint32_t t = kSaveDebounceMs;
  runToIdle(store, f, t);
  const uint8_t* views[2] = {f.read(0), f.read(1)};
  RecordFound found;
  check(newestRecord(views, 2, kRecordSectorBytes, found) && found.rec.sequence == 3 && found.sector == 1 && found.entry == 0,
        "store: the next save moves the log, sequence 3 in entry 0 of the other sector");
  check(f.erases[0] == 0 && f.erases[1] == 0, "store: the blank sector takes it without an erase");
}

// Earlier firmware appended past a torn entry. The boot moves the newest record at once, so the torn
// word faulting later costs nothing.
void checkStoreRecordsPastTear() {
  FakeFlash f;
  for (int e = 0; e < 4; ++e) f.put(0, e, makeRec(1 + static_cast<uint32_t>(e)));
  f.put(0, 4, makeRec(5));
  std::memset(&f.mem[0][4 * kRecordBytes + 3 * kFlashWordBytes], 0x00, 4);  // part-written, reads clean
  for (int e = 5; e < 8; ++e) f.put(0, e, makeRec(10 + static_cast<uint32_t>(e), OutputLevel::Mic));
  RecordStore store(f);
  RecordData out;
  check(store.begin(out) && out.sequence == 17 && store.status().busy,
        "store: newer records past a torn entry load, and a save of the newest is queued");
  uint32_t t = kSaveDebounceMs;
  runToIdle(store, f, t);
  RecordData moved;
  check(decodeRecord(f.mem[1].data(), moved) && moved.sequence == 18 && moved.settings.outputLevel == OutputLevel::Mic,
        "store: the newest record is written again in the other sector");

  const uint32_t fault = f.tear(0, 4, 3);  // the torn word now reads as a double ECC error
  RecordStore reboot(f);
  reboot.recover(0, fault);
  check(reboot.begin(out) && out.sequence == 18 && out.settings.outputLevel == OutputLevel::Mic,
        "store: a later fault on the torn word loses no record");
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

// Polls until the store is idle, the clock jumping a debounce at a time so a queued retry runs.
void runOutRetries(RecordStore& store, uint32_t& t) {
  for (int i = 0; i < 200 && store.status().busy; ++i) {
    store.poll(t);
    t += kSaveDebounceMs / 4;
  }
}

void checkStoreProgramFailure() {
  FakeFlash f;
  RecordStore store(f);
  RecordData out;
  store.begin(out);
  f.failNextProgram = true;
  store.request(defaultLayout(), micOutput(), 0);
  uint32_t t = kSaveDebounceMs;
  store.poll(t);  // sets up the write
  store.poll(t);  // the failed first word
  check(store.status().errors == 1 && store.status().sequence == 0 && store.status().busy,
        "store: a failed program counts an error, keeps the old sequence and queues a retry");
  check(store.takeSaveFailed() && !store.takeSaveFailed(), "store: a failed save is reported once");
  check(store.status().nextEntry == kEntriesPerSector, "store: the sector with the part-written entry takes no more appends");
  store.poll(t + kSaveDebounceMs - 1);
  check(f.programs == 1, "store: the retry waits a debounce");
  t += kSaveDebounceMs;
  runOutRetries(store, t);
  check(store.status().errors == 1 && store.status().sequence == 1 && !store.takeSaveFailed() && f.erases[0] == 1,
        "store: the retry erases the sector it failed in, there being no record to protect, and lands");

  FakeFlash again;  // a start on what that left
  again.mem[0] = f.mem[0];
  RecordStore reboot(again);
  check(reboot.begin(out) && out.sequence == 1 && out.settings.outputLevel == OutputLevel::Mic,
        "store: the retried save loads at the next start");
  check(reboot.bootNotes().dirtySectors == 0 && reboot.bootNotes().tornEntries == 0, "store: and that start reports nothing wrong");
}

// A failed program with a record in flash: the retry goes to the other sector. A second failure is
// left alone until the next request.
void checkStoreRetry() {
  FakeFlash f;
  f.put(0, 0, makeRec(4));
  RecordStore store(f);
  RecordData out;
  store.begin(out);
  f.failNextProgram = true;
  store.request(defaultLayout(), micOutput(), 0);
  uint32_t t = kSaveDebounceMs;
  runOutRetries(store, t);
  RecordData back;
  check(store.status().sequence == 5 && store.status().sector == 1 && decodeRecord(f.mem[1].data(), back) &&
            back.sequence == 5 && back.settings.outputLevel == OutputLevel::Mic && f.erases[0] == 0,
        "store: a failed save is retried once in the other sector, and the newest record stays");

  FakeFlash g;
  RecordStore twice(g);
  twice.begin(out);
  g.failNextProgram = true;
  twice.request(defaultLayout(), micOutput(), 0);
  t = kSaveDebounceMs;
  twice.poll(t);
  twice.poll(t);
  g.failNextProgram = true;  // the retry fails too
  t += kSaveDebounceMs;
  runOutRetries(twice, t);
  const int programs = g.programs;
  check(twice.status().errors == 2 && twice.status().sequence == 0 && !twice.status().busy,
        "store: a retry that fails as well leaves the bank alone");
  twice.request(defaultLayout(), micOutput(), t);
  t += kSaveDebounceMs;
  runOutRetries(twice, t);
  check(g.programs > programs && twice.status().sequence == 1, "store: the next request saves again");
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

// Records firstSeq onward in entries 0 to records - 1 of `sector` and the next record torn at its
// fourth word, the last entry the log wrote. Returns the offset of the faulted word.
uint32_t tornTail(FakeFlash& f, int sector, uint32_t firstSeq, int records) {
  for (int i = 0; i < records; ++i) f.put(sector, i, makeRec(firstSeq + static_cast<uint32_t>(i), OutputLevel::Mic));
  f.put(sector, records, makeRec(firstSeq + static_cast<uint32_t>(records)));
  return f.tear(sector, records, 3);
}

void checkRecoverTornTail() {
  FakeFlash f;
  fillSector(f, 1, 1);  // the older sector, full
  const uint32_t fault = tornTail(f, 0, 1000, 37);
  const uint32_t newest = 1000 + 36;
  RecordStore store(f);
  const RecordRecovery r = store.recover(0, fault);
  check(r.learned && r.carried && !r.carryFailed && !r.eraseFailed && r.sequence == newest,
        "recover: the newest record below the faulted word is carried");
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

// A fault in the very first word, and an offset past the sector, which the clamp reads as the end.
void checkRecoverEdges() {
  FakeFlash f;
  for (int e = 0; e < 3; ++e) f.put(1, e, makeRec(1 + static_cast<uint32_t>(e)));
  const uint32_t first = f.tear(0, 0, 0);
  RecordStore store(f);
  const RecordRecovery r = store.recover(0, first);
  check(first == 0 && r.learned && !r.carried && f.erases[0] == 1 && f.reads[1] == 0,
        "recover: a fault in word 0 of entry 0 erases the sector without reading the other");
  RecordData out;
  check(store.begin(out) && out.sequence == 3, "recover: and the box boots on the other sector");

  FakeFlash g;
  for (int e = 0; e < 5; ++e) g.put(0, e, makeRec(10 + static_cast<uint32_t>(e)));
  RecordStore past(g);
  const RecordRecovery p = past.recover(0, static_cast<uint32_t>(kRecordSectorBytes) + 64);
  check(!p.learned && p.carried && p.sequence == 14 && g.erases[0] == 1,
        "recover: an offset past the sector reads the whole sector, carries its newest and erases it");
  check(past.begin(out) && out.sequence == 14 && past.status().sector == 1, "recover: and the box boots on the copy");
}

// The read limits outlive a fault reset and start over at a power-on or when their seal fails.
void checkReadLimits() {
  FakeFlash f;
  fillSector(f, 1, 1);
  const uint32_t fault = tornTail(f, 0, 1000, 4);  // newest readable: 1003 in entry 3
  f.put(0, 6, makeRec(1100));                      // only a read past the fault could find it
  RecordReadLimits limits;
  std::memset(&limits, 0x5A, sizeof limits);  // DTCM after a power-on
  RecordStore first(f);
  first.useReadLimits(limits, false);
  check(limits.bytes[0] == kRecordSectorBytes && limits.bytes[1] == kRecordSectorBytes,
        "limits: unsealed limits start over with both sectors whole");
  f.failNextErase = true;  // the copy cannot make room, so the faulted sector stays
  const RecordRecovery r = first.recover(0, fault);
  check(r.carryFailed && limits.bytes[0] == 4 * kRecordBytes && limits.bytes[1] == 0,
        "limits: the fault lowers its sector's limit, and a failed copy closes the other sector");

  RecordStore second(f);  // the next start, after a fault reset
  second.useReadLimits(limits, false);
  RecordData out;
  const int readsOther = f.reads[1];
  check(second.begin(out) && out.sequence == 1003 && f.reads[1] == readsOther,
        "limits: the next start reads the faulted sector only below the fault, and the closed one not at all");
  second.request(defaultLayout(), Settings{}, 0);
  uint32_t t = kSaveDebounceMs;
  runToIdle(second, f, t);
  check(second.status().sector == 1 && second.status().sequence == 1004 && limits.bytes[1] == kRecordSectorBytes,
        "limits: the save erases the closed sector, which opens it again");

  RecordStore cold(f);
  cold.useReadLimits(limits, true);
  check(limits.bytes[0] == kRecordSectorBytes, "limits: a power-on opens every sector again");
  limits.bytes[0] = 4 * kRecordBytes;  // changed without its seal
  RecordStore stale(f);
  stale.useReadLimits(limits, false);
  check(limits.bytes[0] == kRecordSectorBytes, "limits: limits that fail their seal start over");
}

// The failed-start count after a start that recovered from its own bank 2 ECC fault.
void checkFailsGiveBack() {
  check(failsAfterEccRecovery(2, true) == 1 && failsAfterEccRecovery(2, false) == 2 && failsAfterEccRecovery(0, true) == 0,
        "limits: a recovery that learned a bad word gives its start back, never below zero");
}

// A later build wrote a record this one cannot decode. The next save still goes past its sequence.
void checkSequenceFloor() {
  for (const bool schema : {true, false}) {
    FakeFlash f;
    for (int e = 0; e < 3; ++e) f.put(0, e, makeRec(1 + static_cast<uint32_t>(e)));
    RecordBytes later = encodeRecord(makeRec(9));
    if (schema) {
      later[kOffSchema] = 3;
    } else {
      later[kOffCleanupEq] = 3;  // a CLEANUP EQ value this build does not have
    }
    reseal(later);
    std::memcpy(&f.mem[0][3 * kRecordBytes], later.data(), kRecordBytes);
    RecordStore store(f);
    RecordData out;
    check(store.begin(out) && out.sequence == 3, "floor: the newest record this build decodes loads");
    store.request(defaultLayout(), micOutput(), 0);
    uint32_t t = kSaveDebounceMs;
    runToIdle(store, f, t);
    check(store.status().sequence == 10,
          schema ? "floor: the save after a schema 3 record takes sequence 10, past it"
                 : "floor: the save after a record with CLEANUP EQ 3 takes sequence 10, past it");
    RecordStore reboot(f);
    check(reboot.begin(out) && out.sequence == 10 && out.settings.outputLevel == OutputLevel::Mic,
          "floor: and that save loads at the next start");
  }
}

// A cut erase after a carried record leaves the same sequence in both sectors.
void checkEqualSequences() {
  FakeFlash f;
  f.put(0, 5, makeRec(7, OutputLevel::Mic));  // the first entries blanked, the rest as it was
  f.put(1, 0, makeRec(7, OutputLevel::Mic));  // the carried copy
  RecordStore store(f);
  RecordData out;
  check(store.begin(out) && out.sequence == 7 && store.status().sector == 1 && store.status().nextEntry == 1,
        "store: on equal sequences the copy in the clean sector wins, and the log goes on there");
  check(store.bootNotes().dirtySectors == 1, "store: the cut erase is reported");
  store.request(defaultLayout(), Settings{}, 0);
  uint32_t t = kSaveDebounceMs;
  runToIdle(store, f, t);
  check(store.status().sequence == 8 && f.erases[0] + f.erases[1] == 0, "store: the next save appends without an erase");
}

// The boot load: a record whose only reverb id is unknown boots the default layout with the
// record's settings and rewrites the record once.
void checkLoadNewest() {
  FakeFlash f;
  RecordStore none(f);
  Layout layout = defaultLayout();
  layout.card[0] = Card::Empty;
  const Layout before = layout;
  Settings settings;
  check(!loadNewestRecord(none, layout, settings, 0).loaded && layout == before, "load: blank flash leaves the layout alone");

  const Layout def = defaultLayout();
  int reverb = -1;
  for (int s = 0; s < kSlotCount; ++s) {
    if (cardDesc(def.card[s]).group == Group::Reverb) reverb = s;
  }
  RecordBytes b = encodeRecord(makeRec(3, OutputLevel::Mic));
  char* id = reinterpret_cast<char*>(&b[kOffSlots + static_cast<size_t>(reverb) * kRecordCardFieldBytes]);
  std::memset(id, 0, kCardIdBytes);
  std::memcpy(id, "no_such_reverb", 14);
  reseal(b);
  std::memcpy(f.mem[0].data(), b.data(), kRecordBytes);
  RecordStore store(f);
  layout = before;
  const RecordLoad load = loadNewestRecord(store, layout, settings, 0);
  check(load.loaded && load.sequence == 3 && load.layout.replaced && load.layout.why.error == LayoutError::NoReverb,
        "load: a record whose reverb id is unknown fails the reverb check");
  check(layout == def && settings.outputLevel == OutputLevel::Mic && store.status().busy,
        "load: the default layout boots with the record's settings, and a save is queued");
  uint32_t t = kSaveDebounceMs;
  runToIdle(store, f, t);
  RecordData back;
  check(store.status().sequence == 4 && decodeRecord(f.mem[0].data() + kRecordBytes, back) && back.layout == def &&
            back.settings.outputLevel == OutputLevel::Mic && !store.status().busy,
        "load: one save rewrites the record with the default layout");

  RecordStore again(f);
  check(!loadNewestRecord(again, layout, settings, 0).layout.replaced && !again.status().busy,
        "load: a valid record queues no save");
}

}  // namespace

void checkRecords() {
  checkRecordCodec();
  checkRecordSchemaV2();
  checkRecordDebugByte();
  checkRecordBrightnessByte();
  checkRecordRetiredBytes();
  checkStoreFlushPending();
  checkRecordUnknownCard();
  checkRecordEntryStates();
  checkRecordNewest();
  checkStoreAppend();
  checkStoreClockSkew();
  checkStoreName();
  checkStoreTornTail();
  checkStoreRecordsPastTear();
  checkStoreSectorSwitch();
  checkStoreInterruptedErase();
  checkStoreEraseAll();
  checkStoreProgramFailure();
  checkStoreRetry();
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
  checkRecoverEdges();
  checkReadLimits();
  checkFailsGiveBack();
  checkSequenceFloor();
  checkEqualSequences();
  checkLoadNewest();
}
