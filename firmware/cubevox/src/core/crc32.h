// CRC-32 as zlib computes it (reflected polynomial 0xEDB88320). The layout record and the image
// header share this one definition; tools/stamp_image.py computes the same value with zlib.

#pragma once

#include <stddef.h>
#include <stdint.h>

// Running state before the first byte. crc32Update() continues a state and crc32Finish() turns the
// last state into the CRC, so a CRC can span several pieces.
constexpr uint32_t kCrc32Init = 0xFFFFFFFFu;

uint32_t crc32Update(uint32_t state, const uint8_t* data, size_t len);

constexpr uint32_t crc32Finish(uint32_t state) { return ~state; }

// CRC-32 of `len` bytes. The input "123456789" gives the standard check value 0xCBF43926.
uint32_t crc32(const uint8_t* data, size_t len);

// CRC-32 of an image with the four bytes at `crcOffset` read as zero, the way the image header
// stores it. Bytes past `length` do not exist, so a field that overruns the image is cut short and
// nothing outside [image, image + length) is read.
uint32_t imageCrc(const uint8_t* image, size_t length, size_t crcOffset);
