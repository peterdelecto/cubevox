// Checks the CRC-32 shared by the layout record and the image header. The golden values come from
// Python's zlib, the library tools/stamp_image.py uses to stamp the image.

#include "support.h"

#include <array>
#include <cstdint>
#include <vector>

#include "core/crc32.h"

namespace {

constexpr size_t kImageBytes = 1100;
constexpr size_t kCrcOffset = 0x40C;  // CV_IMAGE_HEADER_OFFSET + CV_IMAGE_OFFSET_CRC

// A recognisable body with the CRC field set to a nonzero value the check must ignore.
std::vector<uint8_t> makeImage() {
  std::vector<uint8_t> image(kImageBytes);
  for (size_t i = 0; i < image.size(); ++i) image[i] = static_cast<uint8_t>(i * 7 + 3);
  for (size_t i = 0; i < 4; ++i) image[kCrcOffset + i] = 0xA5;
  return image;
}

// Bit by bit, an independent route to the same polynomial.
uint32_t crc32Bitwise(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void checkPlainCrc() {
  const std::array<uint8_t, 9> vec = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  check(crc32(vec.data(), vec.size()) == 0xCBF43926u, "crc32: the standard check value");
  check(crc32(vec.data(), 0) == 0u, "crc32: empty input gives zero");

  const std::vector<uint8_t> image = makeImage();
  check(crc32(image.data(), image.size()) == crc32Bitwise(image.data(), image.size()),
        "crc32: the table agrees with a bitwise CRC on a long buffer");
}

void checkStreaming() {
  const std::vector<uint8_t> image = makeImage();
  const uint32_t whole = crc32(image.data(), image.size());
  bool every = true;
  for (size_t cut = 0; cut <= image.size(); cut += 37) {
    uint32_t state = crc32Update(kCrc32Init, image.data(), cut);
    state = crc32Update(state, image.data() + cut, image.size() - cut);
    every = every && crc32Finish(state) == whole;
  }
  check(every, "crc32: any split of the input gives the one-shot CRC");
}

void checkImageCrc() {
  std::vector<uint8_t> image = makeImage();
  check(imageCrc(image.data(), image.size(), kCrcOffset) == 0xC499FFC3u, "image crc: matches zlib with the field zeroed");

  std::vector<uint8_t> zeroed = image;
  for (size_t i = 0; i < 4; ++i) zeroed[kCrcOffset + i] = 0;
  check(imageCrc(image.data(), image.size(), kCrcOffset) == crc32(zeroed.data(), zeroed.size()),
        "image crc: equals the plain CRC of the image with the field zeroed");
  check(imageCrc(zeroed.data(), zeroed.size(), kCrcOffset) == imageCrc(image.data(), image.size(), kCrcOffset),
        "image crc: the stored field value does not change the result");

  image[kCrcOffset - 1] ^= 1u;
  check(imageCrc(image.data(), image.size(), kCrcOffset) != 0xC499FFC3u, "image crc: a flipped byte before the field shows");
  image[kCrcOffset - 1] ^= 1u;
  image[kCrcOffset + 4] ^= 1u;
  check(imageCrc(image.data(), image.size(), kCrcOffset) != 0xC499FFC3u, "image crc: a flipped byte after the field shows");
}

// The field may overrun the image; the CRC covers what exists and reads nothing past `length`.
void checkImageCrcBounds() {
  const std::vector<uint8_t> image = makeImage();
  check(imageCrc(image.data(), kCrcOffset + 2, kCrcOffset) == 0xBE503790u,
        "image crc: a field cut short by the length zeroes the bytes that exist");
  check(imageCrc(image.data(), kCrcOffset, kCrcOffset) == 0x9CCD0DD0u, "image crc: a field that starts at the length adds nothing");
  check(imageCrc(image.data(), kCrcOffset, kCrcOffset + 100) == 0x9CCD0DD0u,
        "image crc: a field past the length leaves the image whole");
  check(imageCrc(image.data(), 0, 0) == 0u, "image crc: an empty image gives zero");
}

}  // namespace

void checkCrc32() {
  checkPlainCrc();
  checkStreaming();
  checkImageCrc();
  checkImageCrcBounds();
}
