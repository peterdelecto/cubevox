// STM32H743 flash bank 2 as the flash of the layout record and the diagnostic log: 128 KB sectors
// at 0x08100000 and 0x08120000 (the record) and at 0x08140000 (the log). Bank 1 holds the firmware
// and keeps running while bank 2 erases.

#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "board/boot_block.h"
#include "core/record_store.h"

constexpr uint32_t kBank2Base = FLASH_BANK2_BASE;  // bank 2 sector 0

// The sector of the diagnostic log, taken from its address in the bootloader contract.
constexpr int kDiagLogSector = static_cast<int>((CV_DIAG_LOG_ADDR - kBank2Base) / kFlashSectorBytes);
static_assert(kDiagLogSector >= kRecordSectors, "the log must not overlap the record sectors");

// A bound for a stuck controller, not an erase time.
constexpr uint32_t kEraseTimeoutMs = 5000;

// Any failure (a refused unlock, a sector the box does not use, a busy wait that timed out, a
// flash status error) sets a flag that takeError() returns once.
class Bank2Flash : public FlashIo {
 public:
  const uint8_t* read(int sector) const override;
  void eraseStart(int sector) override;
  bool eraseBusy() override;
  void programWord(int sector, uint32_t offset, const uint8_t* flashWord) override;
  bool takeError() override;

  // The sector whose erase is running, or -1. The bank takes one operation at a time, so the
  // layout record and the log each wait while the other one erases.
  int erasingSector() const { return erasing_ ? erasingSector_ : -1; }

 private:
  bool erasing_ = false;
  bool error_ = false;
  int erasingSector_ = 0;
  uint32_t eraseStartMs_ = 0;
};

// The one controller object. The layout record and the diagnostic log share bank 2 through it.
Bank2Flash& bank2Flash();

// Sector index holding a bank 2 address, or -1 outside the sectors the box uses. Sectors 0 and 1
// hold the layout record and sector 2 holds the diagnostic log.
int bank2SectorOf(uint32_t address);
