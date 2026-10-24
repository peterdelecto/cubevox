// The application image check. See image_check.h.

#include "image_check.h"

#include <stdbool.h>
#include <string.h>

#include "text.h"

// Reflected zlib polynomial.
#define CV_CRC_POLY 0xEDB88320u

// Built on first use: 1 KB of RAM instead of 1 KB of flash.
static uint32_t gCrcTable[256];
static bool gCrcTableReady;

static void crcTableBuild(void) {
  for (uint32_t i = 0; i < 256u; ++i) {
    uint32_t c = i;
    for (uint8_t bit = 0; bit < 8u; ++bit) c = (c & 1u) ? (c >> 1) ^ CV_CRC_POLY : c >> 1;
    gCrcTable[i] = c;
  }
  gCrcTableReady = true;
}

uint32_t cvCrc32(uint32_t crc, const uint8_t* data, uint32_t length) {
  if (!gCrcTableReady) crcTableBuild();
  crc = ~crc;
  for (uint32_t i = 0; i < length; ++i) crc = gCrcTable[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  return ~crc;
}

// The CRC the header must carry: the whole image with the CRC field read as zero.
static uint32_t imageCrc(const uint8_t* image, uint32_t length) {
  static const uint8_t kZero[4] = {0u, 0u, 0u, 0u};
  const uint32_t field = CV_IMAGE_HEADER_OFFSET + CV_IMAGE_OFFSET_CRC;
  uint32_t crc = cvCrc32(0u, image, field);
  crc = cvCrc32(crc, kZero, sizeof kZero);
  return cvCrc32(crc, image + field + sizeof kZero, length - field - (uint32_t)sizeof kZero);
}

void cvImageInspect(const uint8_t* image, uint32_t regionBytes, struct CvImageReport* report) {
  struct CvImageHeader header;
  report->state = CvImageNone;
  report->version[0] = '\0';
  if (regionBytes < CV_IMAGE_HEADER_OFFSET + CV_IMAGE_HEADER_SIZE) return;

  memcpy(&header, image + CV_IMAGE_HEADER_OFFSET, sizeof header);
  if (header.magic != CV_IMAGE_MAGIC) return;

  report->state = CvImageBad;
  cvTextPrintable(report->version, header.version, CV_IMAGE_VERSION_BYTES);
  const bool lengthOk = header.length >= CV_IMAGE_HEADER_OFFSET + CV_IMAGE_HEADER_SIZE &&
                        header.length <= regionBytes;
  if (!cvImageHeaderVersionOk(header.headerVersion) || !lengthOk) return;
  if (imageCrc(image, header.length) == header.crc32) report->state = CvImageGood;
}
