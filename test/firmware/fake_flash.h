// In-memory bank 2 for the flash store tests: three 128 KB sectors, 0xFF erased. Programming ANDs
// bits in like real flash. failNextErase and failNextProgram make the next such operation report
// an error. An erase stays busy for eraseBusyPolls polls.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/flash_io.h"
#include "core/record.h"

class FakeFlash : public FlashIo {
 public:
  static constexpr int kSectors = 3;

  FakeFlash() {
    for (std::vector<uint8_t>& sector : mem) sector.assign(kFlashSectorBytes, 0xFF);
  }
  const uint8_t* read(int s) const override { return mem[s].data(); }
  void eraseStart(int s) override {
    erasing = s;
    busyPolls = eraseBusyPolls;
    ++erases[s];
    ++ops;
  }
  bool eraseBusy() override {
    if (erasing < 0) return false;
    if (busyPolls-- > 0) return true;
    if (failNextErase) {
      error = true;
      failNextErase = false;
    } else {
      mem[erasing].assign(kFlashSectorBytes, 0xFF);
    }
    erasing = -1;
    return false;
  }
  void programWord(int s, uint32_t off, const uint8_t* w) override {
    ++programs;
    ++ops;
    if (failNextProgram) {
      error = true;
      failNextProgram = false;
      return;
    }
    for (size_t i = 0; i < kFlashWordBytes; ++i) mem[s][off + i] &= w[i];
  }
  bool takeError() override {
    const bool e = error;
    error = false;
    return e;
  }

  void put(int sector, int entry, const RecordData& rec) {
    const RecordBytes b = encodeRecord(rec);
    std::memcpy(&mem[sector][static_cast<size_t>(entry) * kRecordBytes], b.data(), kRecordBytes);
  }

  std::vector<uint8_t> mem[kSectors];
  int erasing = -1;
  int eraseBusyPolls = 3;
  int busyPolls = 0;
  int erases[kSectors] = {0, 0, 0};
  int programs = 0;
  int ops = 0;
  bool error = false;
  bool failNextErase = false;
  bool failNextProgram = false;
};
