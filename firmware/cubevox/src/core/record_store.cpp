#include "core/record_store.h"

#include <string.h>

#include "core/crc32.h"

namespace {

constexpr uint32_t kReadLimitsMagic = 0x4C524356u;  // "VCRL"

uint32_t entryOffset(int entry) { return static_cast<uint32_t>(entry) * static_cast<uint32_t>(kRecordBytes); }

uint32_t limitsCheck(const RecordReadLimits& l) {
  return crc32(reinterpret_cast<const uint8_t*>(l.bytes), sizeof l.bytes) ^ l.magic;
}

bool limitsSound(const RecordReadLimits& l) {
  if (l.magic != kReadLimitsMagic || l.check != limitsCheck(l)) return false;
  for (const uint32_t bytes : l.bytes) {
    if (bytes > kRecordSectorBytes || bytes % kRecordBytes != 0) return false;
  }
  return true;
}

void seal(RecordReadLimits& l) {
  l.magic = kReadLimitsMagic;
  l.check = limitsCheck(l);
}

void openAll(RecordReadLimits& l) {
  for (uint32_t& bytes : l.bytes) bytes = kRecordSectorBytes;
  seal(l);
}

}  // namespace

RecordStore::RecordStore(FlashIo& io) : io_(io) { openAll(ownLimits_); }

void RecordStore::useReadLimits(RecordReadLimits& limits, bool powerOn) {
  if (powerOn || !limitsSound(limits)) openAll(limits);
  limits_ = &limits;
}

void RecordStore::setReadable(int sector, size_t bytes) {
  limits_->bytes[sector] = static_cast<uint32_t>(bytes);
  seal(*limits_);
}

bool RecordStore::eraseNow(int sector) {
  if (!eraseAndWait(io_, sector)) return false;
  setReadable(sector, kRecordSectorBytes);
  return true;
}

// A clean log is records, then blank entries to the end. A blank entry followed by data means an
// erase was cut off, so the sector is Dirty. A sector read only in part is Dirty too, so it takes no
// appends.
RecordStore::Scan RecordStore::scan(int sector) const {
  Scan s;
  const int entries = static_cast<int>(readable(sector) / kRecordBytes);
  bool dirty = entries < kEntriesPerSector;
  bool anyData = false;
  for (int entry = 0; entry < entries; ++entry) {
    const uint8_t* bytes = io_.view(sector, entryOffset(entry), kRecordBytes);
    if (isErased(bytes, kRecordBytes)) {
      if (s.firstBlank == kEntriesPerSector) s.firstBlank = entry;
      continue;
    }
    RecordData rec;
    uint32_t sequence = 0;
    if (decodeRecord(bytes, rec)) {
      sequence = rec.sequence;
      if (!s.found || rec.sequence > s.newest.rec.sequence) {
        s.newest.rec = rec;
        s.newest.sector = sector;
        s.newest.entry = entry;
        s.found = true;
      }
    } else {
      if (s.torn++ == 0) s.firstTorn = entry;
      if (!recordSealed(bytes, sequence)) sequence = 0;
    }
    if (sequence > s.highest) s.highest = sequence;
    if (s.firstBlank != kEntriesPerSector) dirty = true;
    anyData = true;
  }
  s.state = dirty ? Sector::Dirty : (anyData ? Sector::Log : Sector::Blank);
  return s;
}

// Writes `raw` as the first entry of `sector`, erasing the sector first unless it is blank.
bool RecordStore::copyTo(int sector, Sector state, const RecordBytes& raw) {
  if (state != Sector::Blank && !eraseNow(sector)) return false;
  for (int word = 0; word < kWordsPerRecord; ++word) {
    if (!programEntryWord(io_, sector, 0, raw.data(), word)) return false;
  }
  return true;
}

// Each start reads a sector only up to its limit, and every fault lowers one, so a sector faults at
// most once per power-on however the recoveries and the power cuts between them fall.
RecordRecovery RecordStore::recover(int sector, uint32_t faultOffset) {
  RecordRecovery r;
  if (sector < 0 || sector >= kRecordSectors) return r;
  const int other = 1 - sector;
  const size_t fault = faultOffset < kRecordSectorBytes ? faultOffset : kRecordSectorBytes;
  const size_t below = fault / kRecordBytes * kRecordBytes;  // the entries that end before the word
  r.learned = below < readable(sector);
  if (r.learned) setReadable(sector, below);

  const Scan mine = scan(sector);
  if (mine.found) {
    const Scan theirs = scan(other);
    if (!theirs.found || theirs.newest.rec.sequence < mine.newest.rec.sequence) {
      RecordBytes raw;  // RAM, because the bank stalls reads while it erases and programs
      memcpy(raw.data(), io_.view(sector, entryOffset(mine.newest.entry), kRecordBytes), kRecordBytes);
      r.sequence = mine.newest.rec.sequence;
      r.carried = copyTo(other, theirs.state, raw);
      r.carryFailed = !r.carried;
    }
  }
  if (r.carryFailed) {
    setReadable(other, 0);  // the failed copy left it unknown; the record stays, read up to the fault
    return r;
  }
  r.eraseFailed = !eraseNow(sector);
  if (r.eraseFailed) setReadable(sector, 0);
  return r;
}

bool RecordStore::begin(RecordData& out) {
  std::array<Scan, kRecordSectors> scans;
  notes_ = BootNotes();
  RecordFound found;
  hasRecord_ = false;
  floor_ = 0;
  for (int s = 0; s < kRecordSectors; ++s) {
    scans[s] = scan(s);
    const Scan& here = scans[s];
    state_[s] = here.state;
    notes_.tornEntries += here.torn;
    // A sector read only in part is Dirty for a reason recover() reported.
    if (here.state == Sector::Dirty && readable(s) == kRecordSectorBytes) ++notes_.dirtySectors;
    if (here.highest > floor_) floor_ = here.highest;
    if (!here.found) continue;
    const uint32_t seq = here.newest.rec.sequence;
    // On a tie the copy in a clean sector wins, so the log goes on there.
    const bool cleaner = hasRecord_ && seq == found.rec.sequence && state_[found.sector] == Sector::Dirty &&
                         here.state != Sector::Dirty;
    if (hasRecord_ && seq <= found.rec.sequence && !cleaner) continue;
    found = here.newest;
    hasRecord_ = true;
  }

  if (!hasRecord_) {
    // Nothing to protect: the log starts where it can append, else the next save erases.
    active_ = takesAppends(scans[0]) || !takesAppends(scans[1]) ? 0 : 1;
    nextEntry_ = takesAppends(scans[active_]) ? scans[active_].firstBlank : kEntriesPerSector;
    return false;
  }
  active_ = found.sector;
  newestSector_ = found.sector;
  sequence_ = found.rec.sequence;
  cur_ = found.rec;
  out = found.rec;
  memcpy(stored_.data(), io_.view(found.sector, entryOffset(found.entry), kRecordBytes), kRecordBytes);
  notes_.unknownCard = found.rec.unknownCard;
  // A torn entry or a cut erase stops the appends, so a word that may fault later never sits below
  // a newer record. The next save moves the log to the other sector, and a record already past a
  // torn entry moves there now.
  const Scan& mine = scans[active_];
  nextEntry_ = takesAppends(mine) ? mine.firstBlank : kEntriesPerSector;
  if (mine.firstTorn < found.entry) {
    pending_ = true;
    writeSame_ = true;
    retried_ = false;
    lastRequestMs_ = 0;
  }
  return true;
}

void RecordStore::request(const Layout& layout, const Settings& settings, uint32_t nowMs) {
  cur_.layout = layout;
  cur_.settings = settings;
  pending_ = true;
  retried_ = false;
  lastRequestMs_ = nowMs;
}

void RecordStore::setName(const char* name) {
  strncpy(cur_.name, name, kLayoutNameBytes);  // zero-pads the rest
  cur_.name[kLayoutNameBytes] = '\0';
}

void RecordStore::saveNow() {
  pending_ = true;
  force_ = true;
  writeSame_ = true;
  retried_ = false;
}

bool RecordStore::eraseAll() {
  if (phase_ != Phase::Idle) return false;
  pending_ = false;
  force_ = false;
  writeSame_ = false;
  phase_ = Phase::ErasingAll;
  eraseIdx_ = -1;  // pollEraseAll starts sector 0 on its first call
  return true;
}

void RecordStore::poll(uint32_t nowMs) {
  nowMs_ = nowMs;
  switch (phase_) {
    case Phase::Idle: pollIdle(nowMs); break;
    case Phase::Erasing: pollErase(); break;
    case Phase::Programming: pollProgram(); break;
    case Phase::ErasingAll: pollEraseAll(); break;
  }
}

bool RecordStore::takeSaveFailed() {
  const bool failed = saveFailed_;
  saveFailed_ = false;
  return failed;
}

// The difference is signed so a request stamped later than `nowMs` (two clock reads in one loop
// pass) counts as not yet due instead of wrapping to a huge elapsed time.
void RecordStore::pollIdle(uint32_t nowMs) {
  if (!pending_) return;
  if (!force_ && static_cast<int32_t>(nowMs - lastRequestMs_) < static_cast<int32_t>(kSaveDebounceMs)) return;
  startWrite();
}

// Snapshots the record, skips it when flash already holds the same state, picks the sector, and
// starts an erase when the log has to move. The sequence rises past every record in flash, one this
// build cannot read included.
void RecordStore::startWrite() {
  const bool writeSame = writeSame_;
  pending_ = false;
  force_ = false;
  writeSame_ = false;
  cur_.sequence = (floor_ > sequence_ ? floor_ : sequence_) + 1;
  writing_ = encodeRecord(cur_);
  if (hasRecord_ && !writeSame && sameRecordContent(writing_.data(), stored_.data())) {
    cur_.sequence = sequence_;
    return;
  }
  word_ = 0;

  if (state_[active_] != Sector::Dirty && nextEntry_ < kEntriesPerSector) {
    target_ = active_;
    phase_ = Phase::Programming;
    return;
  }
  // Move to the sector that does not hold the newest record. With no record to protect the same
  // sector is reused.
  target_ = hasRecord_ ? 1 - newestSector_ : active_;
  if (state_[target_] == Sector::Blank) {
    active_ = target_;
    nextEntry_ = 0;
    phase_ = Phase::Programming;
    return;
  }
  io_.eraseStart(target_);
  phase_ = Phase::Erasing;
}

// A failed save is tried once more a debounce later. A bank that fails again is left alone until
// the next request.
void RecordStore::abandon() {
  ++errors_;
  saveFailed_ = true;
  phase_ = Phase::Idle;
  cur_.sequence = sequence_;
  if (retried_) return;
  retried_ = true;
  pending_ = true;
  writeSame_ = true;
  lastRequestMs_ = nowMs_;
}

void RecordStore::pollErase() {
  const EraseStatus erase = checkErase(io_);
  if (erase == EraseStatus::Running) return;
  if (erase == EraseStatus::Failed) {
    state_[target_] = Sector::Dirty;  // not known to be blank
    abandon();
    return;
  }
  state_[target_] = Sector::Blank;
  setReadable(target_, kRecordSectorBytes);
  active_ = target_;
  nextEntry_ = 0;
  phase_ = Phase::Programming;
}

void RecordStore::pollProgram() {
  if (!programEntryWord(io_, target_, entryOffset(nextEntry_), writing_.data(), word_)) {
    // The entry may hold a part-written word, so the sector takes no more appends.
    state_[target_] = Sector::Log;
    nextEntry_ = kEntriesPerSector;
    abandon();
    return;
  }
  if (++word_ < kWordsPerRecord) return;
  state_[target_] = Sector::Log;
  ++nextEntry_;
  newestSector_ = target_;
  sequence_ = cur_.sequence;
  stored_ = writing_;
  hasRecord_ = true;
  phase_ = Phase::Idle;
}

void RecordStore::pollEraseAll() {
  if (eraseIdx_ >= 0) {
    const EraseStatus erase = checkErase(io_);
    if (erase == EraseStatus::Running) return;
    if (erase == EraseStatus::Failed) {
      ++errors_;
      saveFailed_ = true;
      state_[eraseIdx_] = Sector::Dirty;
    } else {
      state_[eraseIdx_] = Sector::Blank;
      setReadable(eraseIdx_, kRecordSectorBytes);
    }
  }
  if (++eraseIdx_ < kRecordSectors) {
    io_.eraseStart(eraseIdx_);
    return;
  }
  hasRecord_ = false;
  active_ = 0;
  nextEntry_ = state_[0] == Sector::Dirty ? kEntriesPerSector : 0;
  sequence_ = 0;
  floor_ = 0;
  phase_ = Phase::Idle;
}

RecordStatus RecordStore::status() const {
  return {hasRecord_, active_, nextEntry_, sequence_, phase_ != Phase::Idle || pending_, errors_};
}

RecordLoad loadNewestRecord(RecordStore& store, Layout& layout, Settings& settings, uint32_t nowMs) {
  RecordLoad load;
  RecordData rec;
  if (!store.begin(rec)) return load;
  load.loaded = true;
  load.sequence = rec.sequence;
  layout = rec.layout;
  settings = rec.settings;
  load.layout = recoverLayout(layout);
  if (load.layout.replaced) store.request(layout, settings, nowMs);
  return load;
}
