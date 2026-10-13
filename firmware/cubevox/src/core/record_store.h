// Append-only log of layout records over the two bank 2 sectors, as a state machine polled from
// loop(). Every poll starts at most one flash operation; nothing here waits on the flash.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/record.h"

constexpr int kRecordSectors = 2;
constexpr size_t kRecordSectorBytes = 128 * 1024;
constexpr int kSlotsPerSector = kRecordSectorBytes / kRecordBytes;
constexpr size_t kFlashWordBytes = 32;
constexpr int kWordsPerRecord = kRecordBytes / kFlashWordBytes;

// A save starts this long after the last request, so turning through the menu writes once.
constexpr uint32_t kSaveDebounceMs = 2000;

// What the store needs from the flash. The board implements it for bank 2, the host test with RAM.
class FlashIo {
 public:
  virtual ~FlashIo() = default;
  // Read-only view of one sector. The store reads it only in begin(), because the same bank
  // stalls reads while it programs or erases.
  virtual const uint8_t* read(int sector) const = 0;
  virtual void eraseStart(int sector) = 0;
  virtual bool eraseBusy() = 0;
  // Programs one 32-byte flash word at `offset` bytes into the sector.
  virtual void programWord(int sector, uint32_t offset, const uint8_t* word) = 0;
  // True once if an erase or program since the last call failed.
  virtual bool takeError() = 0;
};

struct RecordStatus {
  bool hasRecord;
  int sector;     // sector the next append targets
  int nextSlot;   // slot index of the next append
  uint32_t sequence;  // newest valid sequence, 0 when none
  bool busy;
  uint32_t errors;
};

class RecordStore {
 public:
  explicit RecordStore(FlashIo& io) : io_(io) {}

  // Scans both sectors. Returns the newest valid record, or false when there is none.
  bool begin(RecordData& out);

  // Records the state to save and (re)starts the debounce.
  void request(const Layout& layout, const Settings& settings, uint32_t nowMs);
  // The layout name the next save writes; clipped to kRecordNameBytes.
  void setName(const char* name);
  // Writes the current state now, bypassing the debounce.
  void saveNow();
  // Erases both sectors, forgetting all records. Drops the pending save. False while busy.
  bool eraseAll();

  void poll(uint32_t nowMs);

  RecordStatus status() const;
  const RecordData& current() const { return cur_; }

 private:
  enum class Phase : uint8_t { Idle, Erasing, Programming, ErasingAll };
  enum class Sector : uint8_t { Blank, Log, Dirty };

  Sector classify(int sector, int& firstBlank) const;
  void pollIdle(uint32_t nowMs);
  void startWrite();
  void pollErase();
  void pollProgram();
  void pollEraseAll();
  void abandon();

  FlashIo& io_;
  Phase phase_ = Phase::Idle;
  Sector state_[kRecordSectors] = {Sector::Blank, Sector::Blank};
  bool hasRecord_ = false;
  int active_ = 0;      // sector the next append targets
  int nextSlot_ = 0;    // slot of the next append; kSlotsPerSector when the sector takes no more
  uint32_t sequence_ = 0;

  RecordData cur_;
  bool pending_ = false;
  bool force_ = false;
  uint32_t lastRequestMs_ = 0;

  RecordBytes writing_ = {};
  int target_ = 0;   // sector being erased or programmed
  int word_ = 0;
  int eraseIdx_ = 0;
  uint32_t errors_ = 0;
};
