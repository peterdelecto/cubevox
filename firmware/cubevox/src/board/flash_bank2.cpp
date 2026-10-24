#include "board/flash_bank2.h"

#include <Arduino.h>
#include <string.h>

namespace {

// Bank 2 control registers are FLASH->CR2, SR2, CCR2 and KEYR2 (stm32h743xx.h). Sector numbers
// run 0..7 inside the bank, so sector n here is bank 2 sector n.
constexpr uint32_t kKey1 = 0x45670123u;  // FLASH_KEY1 in stm32h7xx_hal_flash.h
constexpr uint32_t kKey2 = 0xCDEF89ABu;
constexpr uint32_t kBusy = FLASH_SR_BSY | FLASH_SR_QW;
constexpr uint32_t kErrors = FLASH_SR_WRPERR | FLASH_SR_PGSERR | FLASH_SR_STRBERR | FLASH_SR_INCERR |
                             FLASH_SR_OPERR | FLASH_SR_RDPERR | FLASH_SR_RDSERR | FLASH_SR_SNECCERR |
                             FLASH_SR_DBECCERR | FLASH_SR_CRCRDERR;
constexpr uint32_t kClearAll = kErrors | FLASH_SR_EOP;  // CCR2 bits sit at the same positions as SR2
constexpr uint32_t kProgramTimeoutUs = 5000;            // one flash word takes tens of microseconds

// The record's two sectors and the log's one. Sectors 3 to 7 are reserved for Adam's data.
bool validSector(int sector) { return (sector >= 0 && sector < kRecordSectors) || sector == kDiagLogSector; }

// True when the control register is unlocked afterwards. A refused key sequence leaves it locked
// until the next reset, so erase and program would silently do nothing.
bool unlock() {
  if ((FLASH->CR2 & FLASH_CR_LOCK) == 0) return true;
  FLASH->KEYR2 = kKey1;
  FLASH->KEYR2 = kKey2;
  return (FLASH->CR2 & FLASH_CR_LOCK) == 0;
}

void lock() { FLASH->CR2 |= FLASH_CR_LOCK; }

// Ends an operation, finished or abandoned: clears the status flags and the mode bit (SER or PG),
// then locks. A mode bit left set would be ORed with the next operation's and raise PGSERR.
void release(uint32_t modeBit) {
  FLASH->CCR2 = kClearAll;
  FLASH->CR2 &= ~modeBit;
  lock();
}

// Drops cached lines so a read sees what the flash holds now. Lines are 32 bytes.
void invalidate(uint32_t address, uint32_t bytes) {
  SCB_InvalidateDCache_by_Addr(reinterpret_cast<uint32_t*>(address), static_cast<int32_t>(bytes));
}

uint32_t sectorAddress(int sector) { return kBank2Base + static_cast<uint32_t>(sector) * kFlashSectorBytes; }

Bank2Flash gBank2;

}  // namespace

Bank2Flash& bank2Flash() { return gBank2; }

int bank2SectorOf(uint32_t address) {
  if (address < kBank2Base) return -1;
  const int index = static_cast<int>((address - kBank2Base) / kFlashSectorBytes);
  return validSector(index) ? index : -1;
}

const uint8_t* Bank2Flash::read(int sector) const { return reinterpret_cast<const uint8_t*>(sectorAddress(sector)); }

void Bank2Flash::eraseStart(int sector) {
  if (!validSector(sector) || !unlock()) {
    error_ = true;
    return;
  }
  FLASH->CCR2 = kClearAll;
  // PSIZE 64 bit (FLASH_VOLTAGE_RANGE_4 in the HAL) is the setting for 2.7 to 3.6 V.
  FLASH->CR2 = (FLASH->CR2 & ~(FLASH_CR_PSIZE | FLASH_CR_SNB)) | FLASH_CR_SER | FLASH_CR_PSIZE |
               (static_cast<uint32_t>(sector) << FLASH_CR_SNB_Pos) | FLASH_CR_START;
  erasingSector_ = sector;
  eraseStartMs_ = millis();
  erasing_ = true;
}

bool Bank2Flash::eraseBusy() {
  if (!erasing_) return false;
  if (FLASH->SR2 & kBusy) {
    if (millis() - eraseStartMs_ < kEraseTimeoutMs) return true;
    error_ = true;
    erasing_ = false;
    release(FLASH_CR_SER);
    return false;
  }

  erasing_ = false;
  if (FLASH->SR2 & kErrors) error_ = true;
  release(FLASH_CR_SER);
  invalidate(sectorAddress(erasingSector_), kFlashSectorBytes);
  return false;
}

void Bank2Flash::programWord(int sector, uint32_t offset, const uint8_t* flashWord) {
  if (!validSector(sector) || offset + kFlashWordBytes > kFlashSectorBytes || !unlock()) {
    error_ = true;
    return;
  }
  const uint32_t address = sectorAddress(sector) + offset;
  FLASH->CCR2 = kClearAll;
  FLASH->CR2 |= FLASH_CR_PG;
  __ISB();
  __DSB();

  volatile uint32_t* dest = reinterpret_cast<volatile uint32_t*>(address);
  for (size_t i = 0; i < kFlashWordBytes / sizeof(uint32_t); ++i) {  // 32-bit writes fill the flash word
    uint32_t v;
    memcpy(&v, flashWord + sizeof v * i, sizeof v);
    dest[i] = v;
  }
  __DSB();

  const uint32_t startUs = micros();
  while ((FLASH->SR2 & kBusy) && micros() - startUs < kProgramTimeoutUs) {}
  const uint32_t status = FLASH->SR2;
  if ((status & kBusy) || (status & kErrors)) error_ = true;
  release(FLASH_CR_PG);
  if (!(status & kBusy)) invalidate(address, kFlashWordBytes);
}

bool Bank2Flash::takeError() {
  const bool e = error_;
  error_ = false;
  return e;
}
