#include "core/entry_log.h"

#include <string.h>

#include "core/crc32.h"
#include "core/le_bytes.h"

namespace {

constexpr size_t kSequenceBytes = 4;
constexpr size_t kCrcBytes = 4;

}  // namespace

EntryLog::EntryLog(FlashIo& io, int sector, size_t entryBytes, size_t sequenceOffset, Gate gate)
    : io_(io), sector_(sector), entryBytes_(entryBytes), sequenceOffset_(sequenceOffset), gate_(gate) {}

bool EntryLog::geometryValid() const {
  if (entryBytes_ <= kCrcBytes || entryBytes_ > kMaxLogEntryBytes) return false;
  if (entryBytes_ % kFlashWordBytes != 0 || kFlashSectorBytes % entryBytes_ != 0) return false;
  return sequenceOffset_ + kSequenceBytes <= entryBytes_ - kCrcBytes;
}

bool EntryLog::entryValid(const uint8_t* entry) const {
  const size_t crcAt = entryBytes_ - kCrcBytes;
  return get32(entry + crcAt) == crc32(entry, crcAt);
}

bool EntryLog::begin() {
  open_ = geometryValid();
  slots_ = 0;
  next_ = 0;
  count_ = 0;
  lastSequence_ = 0;
  if (!open_) return false;
  slots_ = static_cast<uint32_t>(kFlashSectorBytes / entryBytes_);
  scan();
  return true;
}

// A clean log holds entries, then blank slots to the end, and the next write goes to the first
// blank slot. Data after a blank slot means an erase was cut off, so the sector is erased before
// the next write.
void EntryLog::scan() {
  const uint8_t* base = io_.read(sector_);
  uint32_t firstBlank = slots_;
  bool dirty = false;
  for (uint32_t slot = 0; slot < slots_; ++slot) {
    const uint8_t* entry = base + static_cast<size_t>(slot) * entryBytes_;
    if (isErased(entry, entryBytes_)) {
      if (firstBlank == slots_) firstBlank = slot;
      continue;
    }
    if (firstBlank != slots_) dirty = true;
    if (!entryValid(entry)) continue;
    ++count_;
    const uint32_t sequence = get32(entry + sequenceOffset_);
    if (sequence > lastSequence_) lastSequence_ = sequence;
  }
  next_ = dirty ? slots_ : firstBlank;
}

void EntryLog::append(const uint8_t* entry) {
  if (!open_) {
    ++dropped_;
    return;
  }
  if (hasPending_) ++dropped_;
  memcpy(pendingEntry_.data(), entry, entryBytes_);
  hasPending_ = true;
}

void EntryLog::poll() {
  if (gate_ != nullptr && !gate_()) return;
  switch (phase_) {
    case Phase::Idle:
      if (hasPending_) startWrite();
      break;
    case Phase::Erasing: pollErase(); break;
    case Phase::Programming: pollProgram(); break;
  }
}

// Fixes the entry to write, so a later append cannot change it mid-write, and stamps it. The
// erase starts here when the sector has no blank slot left.
void EntryLog::startWrite() {
  writingEntry_ = pendingEntry_;
  hasPending_ = false;
  writingSequence_ = lastSequence_ + 1;
  stamp();
  word_ = 0;
  if (next_ < slots_) {
    phase_ = Phase::Programming;
    return;
  }
  io_.eraseStart(sector_);
  phase_ = Phase::Erasing;
}

void EntryLog::stamp() {
  uint8_t* entry = writingEntry_.data();
  const size_t crcAt = entryBytes_ - kCrcBytes;
  put32(entry + sequenceOffset_, writingSequence_);
  put32(entry + crcAt, crc32(entry, crcAt));
}

void EntryLog::abandon() {
  error_ = true;
  phase_ = Phase::Idle;
}

void EntryLog::pollErase() {
  const EraseStatus erase = checkErase(io_);
  if (erase == EraseStatus::Running) return;
  if (erase == EraseStatus::Failed) {
    abandon();  // next_ stays at slots_, so the next entry erases again
    return;
  }
  next_ = 0;
  count_ = 0;
  phase_ = Phase::Programming;
}

void EntryLog::pollProgram() {
  const uint32_t entryOffset = next_ * static_cast<uint32_t>(entryBytes_);
  if (!programEntryWord(io_, sector_, entryOffset, writingEntry_.data(), word_)) {
    ++next_;  // the slot is part-written and scans as torn
    abandon();
    return;
  }
  if (++word_ < static_cast<int>(entryBytes_ / kFlashWordBytes)) return;
  ++next_;
  ++count_;
  lastSequence_ = writingSequence_;
  phase_ = Phase::Idle;
}

bool EntryLog::flush(uint32_t timeoutMs, uint32_t (*nowMs)(), void (*onPass)()) {
  uint32_t sinceMs = nowMs();
  while (busy()) {
    const uint32_t now = nowMs();
    if (phase_ == Phase::Erasing) {
      sinceMs = now;
    } else if (now - sinceMs >= timeoutMs) {
      return false;
    }
    if (onPass != nullptr) onPass();
    poll();
  }
  return true;
}

bool EntryLog::takeError() {
  const bool failed = error_;
  error_ = false;
  return failed;
}
