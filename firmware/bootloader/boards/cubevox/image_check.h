// The application image check, pure C99 over a memory pointer. The bootloader runs it on flash at
// CV_APP_START; tools/imagecheck.c runs the same code on a file to check it against
// tools/stamp_image.py.

#pragma once

#include <stdint.h>

#include "boot_block.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  // No image magic at the header offset: an erased or foreign region.
  CvImageNone,
  // The magic is there but the header version, the length or the CRC is wrong.
  CvImageBad,
  CvImageGood
} CvImageState;

struct CvImageReport {
  CvImageState state;
  // The header's release name with every non-printable byte as '?', or "" without the magic.
  char version[CV_IMAGE_VERSION_BYTES + 1];
};

// Checks the image at `image` in a region of `regionBytes`: the magic, the header version, the
// length inside the region, then the CRC-32 over the image with the CRC field as zero.
void cvImageInspect(const uint8_t* image, uint32_t regionBytes, struct CvImageReport* report);

// CRC-32 with the zlib polynomial, chained like zlib's crc32(): start with crc 0.
uint32_t cvCrc32(uint32_t crc, const uint8_t* data, uint32_t length);

#ifdef __cplusplus
}
#endif
