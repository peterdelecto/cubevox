#include "core/record_store.h"

#include <string.h>

namespace {

const uint8_t* entryAt(const uint8_t* sector, int entry) { return sector + static_cast<size_t>(entry) * kRecordBytes; }

}  // namespace

// Clean log: records, then blank entries to the end. A blank entry followed by data means an erase
// was cut off, so the sector is Dirty. `firstBlank` is where an append goes (after a torn tail).
// `torn` counts the entries cut off mid-write.
RecordStore::Sector RecordStore::classify(int sector, int& firstBlank, int& torn) const {
  const uint8_t* base = io_.read(sector);
  firstBlank = kEntriesPerSector;
  torn = 0;
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

bool RecordStore::begin(RecordData& out) {
  std::array<int, kRecordSectors> firstBlank{};
  const uint8_t* views[kRecordSectors];
  notes_ = BootNotes();
  for (int s = 0; s < kRecordSectors; ++s) {
    int torn = 0;
    state_[s] = classify(s, firstBlank[s], torn);
    views[s] = io_.read(s);
    notes_.tornEntries += torn;
    if (state_[s] == Sector::Dirty) ++notes_.dirtySectors;
  }

  RecordFound found;
  hasRecord_ = newestRecord(views, kRecordSectors, kRecordSectorBytes, found);
  if (hasRecord_) {
    active_ = found.sector;
    sequence_ = found.rec.sequence;
    cur_ = found.rec;
    out = found.rec;
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
}

bool RecordStore::eraseAll() {
  if (phase_ != Phase::Idle) return false;
  pending_ = false;
  force_ = false;
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

// Picks the sector, snapshots the record, and starts an erase when the log has to move.
void RecordStore::startWrite() {
  pending_ = false;
  force_ = false;
  cur_.sequence = sequence_ + 1;
  writing_ = encodeRecord(cur_);
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
  if (io_.eraseBusy()) return;
  if (io_.takeError()) {
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
  io_.programWord(target_, static_cast<uint32_t>(nextEntry_) * kRecordBytes + word_ * kFlashWordBytes,
                  writing_.data() + word_ * kFlashWordBytes);
  if (io_.takeError()) {
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
  hasRecord_ = true;
  phase_ = Phase::Idle;
}

void RecordStore::pollEraseAll() {
  if (eraseIdx_ >= 0) {
    if (io_.eraseBusy()) return;
    if (io_.takeError()) {
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
