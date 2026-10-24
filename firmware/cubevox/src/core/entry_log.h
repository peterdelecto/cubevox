// Append-only log of fixed-size entries in one flash sector, as a state machine polled from loop().
// Every poll starts at most one flash operation; nothing here waits on the flash.
//
// The caller fills an entry and the log stamps two fields when the write starts. The sequence at
// `sequenceOffset` counts up from 1 and keeps counting across erases. The last four bytes hold the
// CRC-32 of the bytes before them. Both fields are little-endian. A slot of all 0xFF bytes is
// blank, a slot with a matching CRC is valid, and any other slot was cut off mid-write. The scan
// skips torn slots and never repairs them. The flash wears out long before the 32-bit sequence
// wraps, so the log does not handle a wrap.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>

#include "core/flash_io.h"

// The largest entry the log takes.
constexpr size_t kMaxLogEntryBytes = 256;

class EntryLog {
 public:
  // While the gate returns false, poll() starts and continues nothing. The board uses it so the
  // log and the layout record never overlap an erase on their one flash controller.
  using Gate = bool (*)();

  // `entryBytes` is a multiple of the flash word, divides the sector and fits kMaxLogEntryBytes.
  // `sequenceOffset` leaves room for the 4-byte sequence ahead of the CRC.
  EntryLog(FlashIo& io, int sector, size_t entryBytes, size_t sequenceOffset, Gate gate = nullptr);

  // Scans the sector once for the valid entries, the first blank slot and the highest sequence.
  // Returns false when the geometry is unusable, and the log then takes no entries.
  bool begin();

  // Queues one entry of entryBytes bytes. A second entry that arrives before the first has started
  // writing replaces it and counts a drop.
  void append(const uint8_t* entry);

  // Advances the queued entry by one flash operation. With no blank slot left, the sector is
  // erased first and the log starts again at slot 0.
  void poll();

  // True while an entry is queued or being written.
  bool busy() const { return hasPending_ || phase_ != Phase::Idle; }

  // Valid entries in the sector, and the highest sequence among them or written since (0 when none).
  uint32_t count() const { return count_; }
  uint32_t lastSequence() const { return lastSequence_; }

  // Entries replaced in the queue before they started writing, or refused because the log is closed.
  uint32_t dropped() const { return dropped_; }

  // True once after a write or erase failed. The entry in flight is lost.
  bool takeError();

 private:
  enum class Phase : uint8_t { Idle, Erasing, Programming };
  using EntryBytes = std::array<uint8_t, kMaxLogEntryBytes>;

  bool geometryValid() const;
  bool entryValid(const uint8_t* entry) const;
  void scan();
  void startWrite();
  void stamp();
  void pollErase();
  void pollProgram();
  void abandon();

  FlashIo& io_;
  const int sector_;
  const size_t entryBytes_;
  const size_t sequenceOffset_;
  const Gate gate_;

  Phase phase_ = Phase::Idle;
  bool open_ = false;
  uint32_t slots_ = 0;  // entries the sector holds
  uint32_t next_ = 0;   // slot of the next write; slots_ when the sector needs an erase first
  uint32_t count_ = 0;
  uint32_t lastSequence_ = 0;

  bool hasPending_ = false;
  EntryBytes pendingEntry_ = {};
  EntryBytes writingEntry_ = {};
  uint32_t writingSequence_ = 0;
  int word_ = 0;
  uint32_t dropped_ = 0;
  bool error_ = false;
};
