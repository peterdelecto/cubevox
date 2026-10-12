// STM32H743 flash bank 2 as the record store's flash: two 128 KB sectors at 0x08100000 and
// 0x08120000. Bank 1 holds the firmware and keeps running while bank 2 erases.

#pragma once

#include <stdint.h>

#include "core/record_store.h"

constexpr uint32_t kBank2Base = 0x08100000u;  // FLASH_BANK2_BASE, bank 2 sector 0

class Bank2Flash : public FlashIo {
 public:
  const uint8_t* read(int sector) const override;
  void eraseStart(int sector) override;
  bool eraseBusy() override;
  void programWord(int sector, uint32_t offset, const uint8_t* word) override;
  bool takeError() override;

  // Blocking erase for the boot-time ECC recovery only. Never call after the watchdog starts.
  void eraseBlocking(int sector);

 private:
  bool erasing_ = false;
  bool error_ = false;
};

// Sector index (0 or 1) holding a bank 2 address, or -1 outside the record sectors.
int bank2RecordSector(uint32_t address);
