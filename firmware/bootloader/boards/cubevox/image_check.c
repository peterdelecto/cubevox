// The application image check. See image_check.h.

#include "image_check.h"

#include <stdbool.h>
#include <string.h>

#include "../../../cubevox/src/core/crc32_zlib.h"
#include "text.h"

uint32_t cvCrc32(uint32_t crc, const uint8_t* data, uint32_t length) {
  return ~cvCrc32Update(~crc, data, length);
}

uint32_t cvImageCrc(const uint8_t* image, uint32_t length) {
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
  if (cvImageCrc(image, header.length) == header.crc32) report->state = CvImageGood;
}
