#include "board/device_serial.h"

#include <Arduino.h>

namespace {

constexpr size_t kUidBytes = 12;

char gSerial[2 * kUidBytes + 1];
bool gFilled = false;

void fill() {
  constexpr char kHex[] = "0123456789ABCDEF";
  const uint32_t words[kUidBytes / 4] = {HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2()};
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
  return gSerial;
}
