// Append-only log of layout records over the two bank 2 sectors, as a state machine polled from
// loop(). Every poll starts at most one flash operation. Only recover(), which runs at boot, waits
// on the flash.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>

#include "core/flash_io.h"
#include "core/record.h"

constexpr int kRecordSectors = 2;
constexpr size_t kRecordSectorBytes = kFlashSectorBytes;
constexpr int kEntriesPerSector = kRecordSectorBytes / kRecordBytes;
constexpr int kWordsPerRecord = kRecordBytes / kFlashWordBytes;

// A save starts this long after the last change, so a run of menu clicks or a burst of set_layout
// requests writes one record. The sketch skips the wait when the menu closes.
constexpr uint32_t kSaveDebounceMs = 500;

struct RecordStatus {
  bool hasRecord;
  int sector;      // sector the next append targets
  int nextEntry;   // entry index of the next append
  uint32_t sequence;  // newest valid sequence, 0 when none
  bool busy;
  uint32_t errors;
};

// What begin() found wrong in flash or in the newest record, for the board to report.
struct BootNotes {
  bool unknownCard = false;  // the newest record held a card id no card has
  int dirtySectors = 0;      // sectors with data after a blank entry, from an interrupted erase
  int tornEntries = 0;       // entries cut off mid-write, across both sectors
};

// What recover() did about a double ECC error in a record sector.
struct RecordRecovery {
  bool carried = false;      // the newest readable record went to the other sector first
  bool carryFailed = false;  // that copy failed, so the faulted sector stays, read up to the fault
  bool eraseFailed = false;  // the faulted sector would not erase and stays unread
  uint32_t sequence = 0;     // the record carried, or the one whose copy failed
};

class RecordStore {
 public:
  explicit RecordStore(FlashIo& io) : io_(io) {}

  // Boot-time recovery from a double ECC error `faultOffset` bytes into record sector `sector`.
  // Every entry that ends before the faulted flash word was read before the fault, so it reads
  // safely again; the entry holding the word does not. When the newest record among them is newer
  // than everything in the other sector, it is copied there first, so the erase of the faulted
  // sector loses nothing. The other sector is read only in that case. Waits on every erase; call
  // before begin().
  RecordRecovery recover(int sector, uint32_t faultOffset);

  // Scans both sectors, skipping what recover() left unread. Returns the newest valid record, or
  // false when there is none.
  bool begin(RecordData& out);

  // Records the state to save and (re)starts the debounce.
  void request(const Layout& layout, const Settings& settings, uint32_t nowMs);
  // The layout name the next save writes; clipped to kLayoutNameBytes.
  void setName(const char* name);
  // Writes the current state now, bypassing the debounce, even when flash already holds it.
  void saveNow();
  // Skips the debounce for a save that is already pending. Does nothing when none is.
  void flushPending() { force_ = pending_ || force_; }
  // Erases both sectors, forgetting all records. Drops the pending save. False while busy.
  bool eraseAll();

  void poll(uint32_t nowMs);

  // True once after a save or erase failed, so the board can report it.
  bool takeSaveFailed();

  RecordStatus status() const;
  const BootNotes& bootNotes() const { return notes_; }
  const RecordData& current() const { return cur_; }

 private:
  enum class Phase : uint8_t { Idle, Erasing, Programming, ErasingAll };
  enum class Sector : uint8_t { Blank, Log, Dirty };

  Sector classify(int sector, int& firstBlank, int& torn) const;
  bool newestIn(int sector, size_t bytes, RecordFound& found) const;
  bool olderElsewhere(int sector, uint32_t sequence) const;
  bool copyTo(int sector, const RecordBytes& raw);
  void pollIdle(uint32_t nowMs);
  void startWrite();
  void pollErase();
  void pollProgram();
  void pollEraseAll();
  void abandon();

  FlashIo& io_;
  Phase phase_ = Phase::Idle;
  std::array<Sector, kRecordSectors> state_ = {Sector::Blank, Sector::Blank};
  // Bytes from the start of each sector that begin() may read. recover() lowers it for a sector
  // that still holds a faulting word.
  std::array<size_t, kRecordSectors> readable_ = {kRecordSectorBytes, kRecordSectorBytes};
  bool hasRecord_ = false;
  int active_ = 0;      // sector the next append targets
  int nextEntry_ = 0;   // entry of the next append; kEntriesPerSector when the sector takes no more
  uint32_t sequence_ = 0;

  RecordData cur_;
  bool pending_ = false;
  bool force_ = false;      // skip the debounce
  bool writeSame_ = false;  // write even when flash already holds the state
  uint32_t lastRequestMs_ = 0;

  RecordBytes stored_ = {};  // the newest record as flash holds it, while hasRecord_
  RecordBytes writing_ = {};
  int target_ = 0;   // sector being erased or programmed
  int word_ = 0;
  int eraseIdx_ = 0;
  uint32_t errors_ = 0;
  bool saveFailed_ = false;
  BootNotes notes_;
};
