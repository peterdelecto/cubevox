#include "core/flash_io.h"

#include <string.h>

bool isErased(const uint8_t* data, size_t len) {
  size_t i = 0;
  for (; i + sizeof(uint32_t) <= len; i += sizeof(uint32_t)) {
    uint32_t word;
    memcpy(&word, data + i, sizeof word);
    if (word != 0xFFFFFFFFu) return false;
  }
  for (; i < len; ++i) {
    if (data[i] != 0xFF) return false;
  }
  return true;
}

bool programEntryWord(FlashIo& io, int sector, uint32_t entryOffset, const uint8_t* entry, int word) {
  const uint32_t at = static_cast<uint32_t>(word) * static_cast<uint32_t>(kFlashWordBytes);
  io.programWord(sector, entryOffset + at, entry + at);
  return !io.takeError();
}

EraseStatus checkErase(FlashIo& io) {
  if (io.eraseBusy()) return EraseStatus::Running;
  return io.takeError() ? EraseStatus::Failed : EraseStatus::Done;
}

bool eraseAndWait(FlashIo& io, int sector) {
  io.eraseStart(sector);
  EraseStatus status = checkErase(io);
  while (status == EraseStatus::Running) status = checkErase(io);
  return status == EraseStatus::Done;
}
