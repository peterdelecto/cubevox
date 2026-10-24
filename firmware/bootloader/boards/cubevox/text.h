// Text helpers the bootloader's C files share: hex and decimal digits and the printable filter. No
// HAL and no printf, so the host tools and tests compile them unchanged.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The uppercase hex digit for the low nibble of `value`.
static inline char cvTextHexDigit(uint32_t value) {
  return "0123456789ABCDEF"[value & 0xFu];
}

// `c` when it is printable ASCII, else '?'.
static inline char cvTextPrintableChar(char c) {
  return (c >= 0x20 && c < 0x7F) ? c : '?';
}

// Copies a NUL padded text field of `size` bytes to out[size + 1], each byte through
// cvTextPrintableChar. Returns the length.
static inline uint32_t cvTextPrintable(char* out, const char* field, uint32_t size) {
  uint32_t n = 0u;
  for (; n < size && field[n] != '\0'; ++n) out[n] = cvTextPrintableChar(field[n]);
  out[n] = '\0';
  return n;
}

// Writes `value` in decimal into out[11] and returns where the digits start.
static inline const char* cvTextDecimal(char out[11], uint32_t value) {
  uint32_t n = 10u;
  out[n] = '\0';
  do {
    out[--n] = (char)('0' + value % 10u);
    value /= 10u;
  } while (value != 0u && n > 0u);
  return &out[n];
}

#ifdef __cplusplus
}
#endif
