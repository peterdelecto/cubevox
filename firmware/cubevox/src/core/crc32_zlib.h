// The CRC-32 step of zlib (reflected polynomial 0xEDB88320), one definition for the application
// (core/crc32.cpp) and the bootloader (boards/cubevox/image_check.c). Valid C99 and C++. A nibble
// table keeps it to 64 bytes of flash and no RAM, and the boot-time check of a 180 KB image still
// takes a few milliseconds.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define CV_CRC32_INLINE inline
#else
#define CV_CRC32_INLINE static inline
#endif

// Continues a running state over `length` bytes. The state starts at 0xFFFFFFFF and the CRC is its
// complement.
CV_CRC32_INLINE uint32_t cvCrc32Update(uint32_t state, const uint8_t* data, size_t length) {
  static const uint32_t kNibble[16] = {0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
                                       0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
                                       0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
                                       0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
  for (size_t i = 0; i < length; ++i) {
    state ^= data[i];
    state = kNibble[state & 0xFu] ^ (state >> 4);
    state = kNibble[state & 0xFu] ^ (state >> 4);
  }
  return state;
}
