#include "board/device_serial.h"

#include <Arduino.h>

#include <array>

namespace {

constexpr size_t kUidBytes = 12;
constexpr size_t kUidWords = kUidBytes / sizeof(uint32_t);

std::array<char, 2 * kUidBytes + 1> gSerial = {};
bool gFilled = false;

void fill() {
  constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                         '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  const std::array<uint32_t, kUidWords> words = {HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2()};
  for (size_t i = 0; i < kUidBytes; ++i) {
    const uint8_t byte = static_cast<uint8_t>(words[i / 4] >> (8 * (i % 4)));  // little-endian: address order
    gSerial[2 * i] = kHex[byte >> 4];
    gSerial[2 * i + 1] = kHex[byte & 0xFu];
  }
  gSerial[2 * kUidBytes] = '\0';
  gFilled = true;
}

}  // namespace

const char* deviceSerial() {
  if (!gFilled) fill();
  return gSerial.data();
}
