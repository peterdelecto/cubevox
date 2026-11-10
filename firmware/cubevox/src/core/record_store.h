// Append-only log of layout records over the two bank 2 sectors, as a state machine polled from
// loop(). Every poll starts at most one flash operation, and a program waits at most 5 ms for its
// word. recover(), which runs at boot, waits on whole erases.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>

#include "core/flash_io.h"
#include "core/layout.h"
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
  bool learned = false;      // the fault was new, so the sector's read limit came down to it
  bool carried = false;      // the newest readable record went to the other sector first
  bool carryFailed = false;  // that copy failed, so the faulted sector stays, read up to the fault
  bool eraseFailed = false;  // the faulted sector would not erase and stays unread
  uint32_t sequence = 0;     // the record carried, or the one whose copy failed
};

// Bytes from the start of each record sector a start may read. The board keeps them in .cv_noinit,
// so they outlive a fault reset. A double ECC error lowers its sector's limit to the start of the
// entry holding the word, so no later start reads that word again, and a finished erase of the
// sector lifts the limit. RecordStore::useReadLimits() seals and checks them.
struct RecordReadLimits {
  uint32_t magic;
  uint32_t bytes[kRecordSectors];
  uint32_t check;
};

// The bootloader's failed-start count after a start that recovered from its own bank 2 ECC fault.
// A recovery that learned a new bad word gives that start back, because every later start reads
// less. Each sector has at most kEntriesPerSector limits to fall through, so the give-back ends.
constexpr uint32_t failsAfterEccRecovery(uint32_t fails, bool learned) {
  return learned && fails > 0 ? fails - 1 : fails;
}

class RecordStore {
 public:
  explicit RecordStore(FlashIo& io);

  // Reads and keeps the read limits in `limits`, which outlive a reset. A power-on, or limits that
  // fail their seal, start over with both sectors readable in full. Call before recover().
  void useReadLimits(RecordReadLimits& limits, bool powerOn);

  // Boot-time recovery from a double ECC error `faultOffset` bytes into record sector `sector`.
  // The sector's read limit comes down to the entry holding the word; the entries below it were
  // read before the fault, so they read safely again. When the newest record among them is newer
  // than everything readable in the other sector, it is copied there first, so the erase of the
  // faulted sector loses nothing. The other sector is read only in that case. Waits on every
  // erase; call before begin().
  RecordRecovery recover(int sector, uint32_t faultOffset);

  // Scans both sectors up to their read limits. Returns the newest valid record, or false when
  // there is none. Newer records left past a torn entry are saved again in the other sector.
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

  // True once after a save or erase failed, so the board can report it. A failed save is tried
  // once more a debounce later.
  bool takeSaveFailed();

  RecordStatus status() const;
  const BootNotes& bootNotes() const { return notes_; }
  const RecordData& current() const { return cur_; }

 private:
  enum class Phase : uint8_t { Idle, Erasing, Programming, ErasingAll };
  enum class Sector : uint8_t { Blank, Log, Dirty };

  // One sector read up to its limit.
  struct Scan {
    Sector state = Sector::Blank;
    int firstBlank = kEntriesPerSector;
    int torn = 0;                       // entries that are neither blank nor a record this build reads
    int firstTorn = kEntriesPerSector;
    bool found = false;
    RecordFound newest;                 // the newest valid record, when found
    uint32_t highest = 0;               // the highest sequence with a good magic and CRC
  };

  Scan scan(int sector) const;
  static bool takesAppends(const Scan& s) { return s.state != Sector::Dirty && s.torn == 0; }
  size_t readable(int sector) const { return limits_->bytes[sector]; }
  void setReadable(int sector, size_t bytes);
  bool eraseNow(int sector);
  bool copyTo(int sector, Sector state, const RecordBytes& raw);
  void pollIdle(uint32_t nowMs);
  void startWrite();
  void pollErase();
  void pollProgram();
  void pollEraseAll();
  void abandon();

  FlashIo& io_;
  RecordReadLimits ownLimits_;
  RecordReadLimits* limits_ = &ownLimits_;
  Phase phase_ = Phase::Idle;
  std::array<Sector, kRecordSectors> state_ = {Sector::Blank, Sector::Blank};
  bool hasRecord_ = false;
  int active_ = 0;        // sector the next append targets
  int nextEntry_ = 0;     // entry of the next append; kEntriesPerSector when the sector takes no more
  int newestSector_ = 0;  // sector holding stored_, which a move never erases
  uint32_t sequence_ = 0;
  uint32_t floor_ = 0;    // highest sequence in flash, a record this build cannot read included

  RecordData cur_;
  bool pending_ = false;
  bool force_ = false;      // skip the debounce
  bool writeSame_ = false;  // write even when flash already holds the state
  bool retried_ = false;    // the current request has had its one retry
  uint32_t lastRequestMs_ = 0;
  uint32_t nowMs_ = 0;

  RecordBytes stored_ = {};  // the newest record as flash holds it, while hasRecord_
  RecordBytes writing_ = {};
  int target_ = 0;   // sector being erased or programmed
  int word_ = 0;
  int eraseIdx_ = 0;
  uint32_t errors_ = 0;
  bool saveFailed_ = false;
  BootNotes notes_;
};

// What the boot load did with the newest record.
struct RecordLoad {
  bool loaded = false;   // a record was found, and `layout` and `settings` hold it
  uint32_t sequence = 0;
  Recovery layout;       // replaced when the record's layout failed a check
};

// Loads the newest record into `layout` and `settings`, and leaves both untouched when there is
// none. A layout that fails a check becomes the default layout with the record's settings, and a
// save rewrites the record with it.
RecordLoad loadNewestRecord(RecordStore& store, Layout& layout, Settings& settings, uint32_t nowMs);
