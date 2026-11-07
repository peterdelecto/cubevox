#include "core/crc32.h"

#include <array>

#include "core/crc32_zlib.h"

namespace {

constexpr size_t kCrcFieldBytes = 4;

constexpr std::array<uint8_t, kCrcFieldBytes> kZeroField = {};

}  // namespace

uint32_t crc32Update(uint32_t state, const uint8_t* data, size_t len) { return cvCrc32Update(state, data, len); }

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
