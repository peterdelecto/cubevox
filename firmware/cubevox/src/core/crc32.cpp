#include "core/crc32.h"

#include <array>

namespace {

constexpr uint32_t kPolynomial = 0xEDB88320u;
constexpr size_t kCrcFieldBytes = 4;

constexpr std::array<uint32_t, 256> makeTable() {
  std::array<uint32_t, 256> table = {};
  for (uint32_t i = 0; i < table.size(); ++i) {
    uint32_t entry = i;
    for (int bit = 0; bit < 8; ++bit) entry = (entry & 1u) != 0 ? (entry >> 1) ^ kPolynomial : entry >> 1;
    table[i] = entry;
  }
  return table;
}

// A byte per step keeps the boot-time check of a 180 KB image to a few milliseconds.
constexpr std::array<uint32_t, 256> kTable = makeTable();

constexpr std::array<uint8_t, kCrcFieldBytes> kZeroField = {};

}  // namespace

uint32_t crc32Update(uint32_t state, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) state = kTable[(state ^ data[i]) & 0xFFu] ^ (state >> 8);
  return state;
}

uint32_t crc32(const uint8_t* data, size_t len) { return crc32Finish(crc32Update(kCrc32Init, data, len)); }

uint32_t imageCrc(const uint8_t* image, size_t length, size_t crcOffset) {
  const size_t head = crcOffset < length ? crcOffset : length;
  const size_t field = length - head < kCrcFieldBytes ? length - head : kCrcFieldBytes;
  uint32_t state = crc32Update(kCrc32Init, image, head);
  state = crc32Update(state, kZeroField.data(), field);
  const size_t tail = head + kCrcFieldBytes;
  if (tail < length) state = crc32Update(state, image + tail, length - tail);
  return crc32Finish(state);
}
