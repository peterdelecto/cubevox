#include "core/record_store.h"

#include <string.h>

namespace {

const uint8_t* entryAt(const uint8_t* sector, int entry) { return sector + static_cast<size_t>(entry) * kRecordBytes; }

}  // namespace

// Clean log: records, then blank entries to the end. A blank entry followed by data means an erase
// was cut off, so the sector is Dirty. `firstBlank` is where an append goes (after a torn tail).
// `torn` counts the entries cut off mid-write. A sector begin() may read only in part is Dirty too,
// so it takes no appends.
RecordStore::Sector RecordStore::classify(int sector, int& firstBlank, int& torn) const {
  firstBlank = kEntriesPerSector;
  torn = 0;
  if (readable_[sector] < kRecordSectorBytes) return Sector::Dirty;
  const uint8_t* base = io_.read(sector);
  bool dirty = false;
  bool anyData = false;
  for (int entry = 0; entry < kEntriesPerSector; ++entry) {
    const EntryState s = classifyRecordEntry(entryAt(base, entry));
    if (s == EntryState::Blank) {
      if (firstBlank == kEntriesPerSector) firstBlank = entry;
      continue;
    }
    if (s == EntryState::Torn) ++torn;
    if (firstBlank != kEntriesPerSector) dirty = true;
    anyData = true;
  }
  if (dirty) return Sector::Dirty;
  return anyData ? Sector::Log : Sector::Blank;
}

// The newest valid record among the first `bytes` of `sector`.
bool RecordStore::newestIn(int sector, size_t bytes, RecordFound& found) const {
  if (bytes == 0) return false;
  const uint8_t* view = io_.read(sector);
  if (!newestRecord(&view, 1, bytes, found)) return false;
  found.sector = sector;
  return true;
}

// True when `sector` holds no record as new as `sequence`.
bool RecordStore::olderElsewhere(int sector, uint32_t sequence) const {
  RecordFound theirs;
  return !newestIn(sector, kRecordSectorBytes, theirs) || theirs.rec.sequence < sequence;
}

// Writes `raw` as the first entry of `sector`, erasing the sector first unless it is blank. With
// nothing below the copy, a power cut that tears it leaves that sector no record to carry, so its
// own recovery erases it without reading the sector the record came from, whose faulting word is
// still there. Two recoveries therefore never fault on each other's sector in turn.
bool RecordStore::copyTo(int sector, const RecordBytes& raw) {
  int firstBlank = 0;
  int torn = 0;
  if (classify(sector, firstBlank, torn) != Sector::Blank && !eraseAndWait(io_, sector)) return false;
  for (int word = 0; word < kWordsPerRecord; ++word) {
    if (!programEntryWord(io_, sector, 0, raw.data(), word)) return false;
  }
  return true;
}

RecordRecovery RecordStore::recover(int sector, uint32_t faultOffset) {
  RecordRecovery r;
  if (sector < 0 || sector >= kRecordSectors) return r;
  const int other = 1 - sector;
  const size_t fault = faultOffset < kRecordSectorBytes ? faultOffset : kRecordSectorBytes;
  const size_t readable = fault / kRecordBytes * kRecordBytes;  // the entries that end before the word
  RecordFound mine;
  if (newestIn(sector, readable, mine) && olderElsewhere(other, mine.rec.sequence)) {
    RecordBytes raw;  // RAM, because the bank stalls reads while it erases and programs
    memcpy(raw.data(), entryAt(io_.read(sector), mine.entry), kRecordBytes);
    r.sequence = mine.rec.sequence;
    r.carried = copyTo(other, raw);
    r.carryFailed = !r.carried;
  }
  if (r.carryFailed) {
    readable_[sector] = readable;  // the record stays put, and begin() reads up to the fault
    readable_[other] = 0;          // the failed copy left it unknown
    return r;
  }
  r.eraseFailed = !eraseAndWait(io_, sector);
  if (r.eraseFailed) readable_[sector] = 0;
  return r;
}

bool RecordStore::begin(RecordData& out) {
  std::array<int, kRecordSectors> firstBlank{};
  notes_ = BootNotes();
  RecordFound found;
  hasRecord_ = false;
  for (int s = 0; s < kRecordSectors; ++s) {
    int torn = 0;
    state_[s] = classify(s, firstBlank[s], torn);
    notes_.tornEntries += torn;
    // A sector left unread is Dirty without having been read; recover() reported why.
    if (state_[s] == Sector::Dirty && readable_[s] == kRecordSectorBytes) ++notes_.dirtySectors;
    RecordFound here;
    if (!newestIn(s, readable_[s], here) || (hasRecord_ && here.rec.sequence <= found.rec.sequence)) continue;
    found = here;
    hasRecord_ = true;
  }

  if (hasRecord_) {
    active_ = found.sector;
    sequence_ = found.rec.sequence;
    cur_ = found.rec;
    out = found.rec;
    memcpy(stored_.data(), entryAt(io_.read(found.sector), found.entry), kRecordBytes);
    notes_.unknownCard = found.rec.unknownCard;
    // A dirty sector takes no appends; the next save moves to the other sector.
    nextEntry_ = state_[active_] == Sector::Dirty ? kEntriesPerSector : firstBlank[active_];
    return true;
  }
  active_ = state_[0] != Sector::Dirty ? 0 : 1;
  nextEntry_ = state_[active_] == Sector::Dirty ? kEntriesPerSector : firstBlank[active_];
  return false;
}

void RecordStore::request(const Layout& layout, const Settings& settings, uint32_t nowMs) {
  cur_.layout = layout;
  cur_.settings = settings;
  pending_ = true;
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
// starts an erase when the log has to move.
void RecordStore::startWrite() {
  const bool writeSame = writeSame_;
  pending_ = false;
  force_ = false;
  writeSame_ = false;
  cur_.sequence = sequence_ + 1;
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
  // Move to a fresh sector. With no record to protect the same sector is reused.
  target_ = hasRecord_ ? 1 - active_ : active_;
  if (state_[target_] == Sector::Blank) {
    active_ = target_;
    nextEntry_ = 0;
    phase_ = Phase::Programming;
    return;
  }
  io_.eraseStart(target_);
  phase_ = Phase::Erasing;
}

void RecordStore::abandon() {
  ++errors_;
  saveFailed_ = true;
  phase_ = Phase::Idle;
  cur_.sequence = sequence_;
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
  active_ = target_;
  nextEntry_ = 0;
  phase_ = Phase::Programming;
}

void RecordStore::pollProgram() {
  const uint32_t entryOffset = static_cast<uint32_t>(nextEntry_) * static_cast<uint32_t>(kRecordBytes);
  if (!programEntryWord(io_, target_, entryOffset, writing_.data(), word_)) {
    // The entry is part-written; skip it and drop this save.
    state_[target_] = Sector::Log;
    ++nextEntry_;
    abandon();
    return;
  }
  if (++word_ < kWordsPerRecord) return;
  state_[target_] = Sector::Log;
  ++nextEntry_;
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
  phase_ = Phase::Idle;
}

RecordStatus RecordStore::status() const {
  return {hasRecord_, active_, nextEntry_, sequence_, phase_ != Phase::Idle || pending_, errors_};
}
