// The "label=0x..." field of the fault and boot report lines.

#pragma once

#include <Arduino.h>
#include <stdint.h>

inline void printHex(Print& out, const char* label, uint32_t v) {
  out.print(label);
  out.print("=0x");
  out.print(v, HEX);
}
