#include "board/flash_bank2.h"

#include <Arduino.h>
#include <string.h>

namespace {

// Bank 2 control registers are FLASH->CR2, SR2, CCR2 and KEYR2 (stm32h743xx.h). Sector numbers
// run 0..7 inside the bank, so sector n of the record is bank 2 sector n.
constexpr uint32_t kKey1 = 0x45670123u;  // FLASH_KEY1 in stm32h7xx_hal_flash.h
constexpr uint32_t kKey2 = 0xCDEF89ABu;
constexpr uint32_t kBusy = FLASH_SR_BSY | FLASH_SR_QW;
constexpr uint32_t kErrors = FLASH_SR_WRPERR | FLASH_SR_PGSERR | FLASH_SR_STRBERR | FLASH_SR_INCERR |
                             FLASH_SR_OPERR | FLASH_SR_RDPERR | FLASH_SR_RDSERR | FLASH_SR_SNECCERR |
                             FLASH_SR_DBECCERR | FLASH_SR_CRCRDERR;
constexpr uint32_t kClearAll = kErrors | FLASH_SR_EOP;  // CCR2 bits sit at the same positions as SR2
constexpr uint32_t kProgramTimeoutUs = 5000;            // one flash word takes tens of microseconds

void unlock() {
  if ((FLASH->CR2 & FLASH_CR_LOCK) == 0) return;
  FLASH->KEYR2 = kKey1;
  FLASH->KEYR2 = kKey2;
}

void lock() { FLASH->CR2 |= FLASH_CR_LOCK; }

// Drops cached lines so a read sees what the flash holds now. Lines are 32 bytes.
void invalidate(uint32_t address, uint32_t bytes) {
  SCB_InvalidateDCache_by_Addr(reinterpret_cast<uint32_t*>(address), static_cast<int32_t>(bytes));
}

uint32_t sectorAddress(int sector) { return kBank2Base + static_cast<uint32_t>(sector) * kRecordSectorBytes; }

}  // namespace

int bank2RecordSector(uint32_t address) {
  if (address < kBank2Base) return -1;
  const uint32_t index = (address - kBank2Base) / kRecordSectorBytes;
  return index < kRecordSectors ? static_cast<int>(index) : -1;
}

const uint8_t* Bank2Flash::read(int sector) const { return reinterpret_cast<const uint8_t*>(sectorAddress(sector)); }

void Bank2Flash::eraseStart(int sector) {
  unlock();
  FLASH->CCR2 = kClearAll;
  // PSIZE 64 bit (FLASH_VOLTAGE_RANGE_4 in the HAL) is the setting for 2.7 to 3.6 V.
  FLASH->CR2 = (FLASH->CR2 & ~(FLASH_CR_PSIZE | FLASH_CR_SNB)) | FLASH_CR_SER | FLASH_CR_PSIZE |
               (static_cast<uint32_t>(sector) << FLASH_CR_SNB_Pos) | FLASH_CR_START;
  erasing_ = true;
}

bool Bank2Flash::eraseBusy() {
  if (!erasing_) return false;
  if (FLASH->SR2 & kBusy) return true;

  erasing_ = false;
  if (FLASH->SR2 & kErrors) error_ = true;
  FLASH->CCR2 = kClearAll;
  FLASH->CR2 &= ~FLASH_CR_SER;
  lock();
  for (int s = 0; s < kRecordSectors; ++s) invalidate(sectorAddress(s), kRecordSectorBytes);
  return false;
}

void Bank2Flash::programWord(int sector, uint32_t offset, const uint8_t* word) {
  const uint32_t address = sectorAddress(sector) + offset;
  unlock();
  FLASH->CCR2 = kClearAll;
  FLASH->CR2 |= FLASH_CR_PG;
  __ISB();
  __DSB();

  volatile uint32_t* dest = reinterpret_cast<volatile uint32_t*>(address);
  for (size_t i = 0; i < kFlashWordBytes / sizeof(uint32_t); ++i) {  // 32-bit writes fill the flash word
    uint32_t v;
    memcpy(&v, word + sizeof v * i, sizeof v);
    dest[i] = v;
  }
  __DSB();

  const uint32_t startUs = micros();
  while ((FLASH->SR2 & kBusy) && micros() - startUs < kProgramTimeoutUs) {}
  if ((FLASH->SR2 & (kBusy | kErrors)) != 0) error_ = true;
  FLASH->CCR2 = kClearAll;
  FLASH->CR2 &= ~FLASH_CR_PG;
  lock();
  invalidate(address, kFlashWordBytes);
}

bool Bank2Flash::takeError() {
  const bool e = error_;
  error_ = false;
  return e;
}

void Bank2Flash::eraseBlocking(int sector) {
  eraseStart(sector);
  while (eraseBusy()) {}
}
