// What the flash stores need from the flash, and the steps both stores take on it. The board
// implements FlashIo for bank 2 and the host test with RAM.

#pragma once

#include <stddef.h>
#include <stdint.h>

// The flash programs whole words of this size.
constexpr size_t kFlashWordBytes = 32;

// One erase unit. The H743 has 128 KB sectors.
constexpr size_t kFlashSectorBytes = 128 * 1024;

class FlashIo {
 public:
  virtual ~FlashIo() = default;
  // Read-only view of one sector. A store reads it only in begin(), because the same bank
  // stalls reads while it programs or erases.
  virtual const uint8_t* read(int sector) const = 0;
  virtual void eraseStart(int sector) = 0;
  // True while the erase runs. An implementation gives up on its own when the flash hangs.
  virtual bool eraseBusy() = 0;
  // Programs one flash word (kFlashWordBytes) at `offset` bytes into the sector.
  virtual void programWord(int sector, uint32_t offset, const uint8_t* word) = 0;
  // True once if an erase or program since the last call failed.
  virtual bool takeError() = 0;
};

// True when every byte of `data` is 0xFF, the erased state.
bool isErased(const uint8_t* data, size_t len);

// Programs word `word` of the entry that starts `entryOffset` bytes into `sector`. Returns false
// when the flash reported an error.
bool programEntryWord(FlashIo& io, int sector, uint32_t entryOffset, const uint8_t* entry, int word);

enum class EraseStatus : uint8_t { Running, Done, Failed };

// One look at the erase begun with eraseStart(). Failed takes the flash's error flag.
EraseStatus checkErase(FlashIo& io);

// Erases `sector` and waits for the result, as long as eraseBusy() takes to give up on a hung flash.
// For the boot-time ECC recovery only, which runs before the application's watchdog. Returns false
// when the erase failed.
bool eraseAndWait(FlashIo& io, int sector);
