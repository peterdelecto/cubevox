// The header each image carries at CV_APP_START + CV_IMAGE_HEADER_OFFSET (spec 2026-10-24, Image
// header) and the check of the running image against it.

#pragma once

#include "board/boot_block.h"

// The compiler emits length and CRC as zero, and the stamp step of firmware/build.sh fills both in
// the .bin after the link. The copy in flash therefore differs from this constant in those two
// fields, and readers of the stamped values go through the flash address.
extern const CvImageHeader kImageHeader;

// The release name in the header, such as v2.00.10 or v2.00.10-bench.
const char* imageVersion();

// True when the image in flash matches its stamped header: magic, header version, a length inside
// the application region, and the CRC of every byte of the image. The first call reads the whole
// image, a few milliseconds, and keeps the answer.
bool imageCrcOk();
