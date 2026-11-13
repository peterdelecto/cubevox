// The updater's checks. See updater_check.h.

#include "updater_check.h"

#include <string.h>

#include "image_check.h"

// Sector 0, where the carried bootloader runs, and the DTCM its stack starts in.
#define CV_SECTOR0_START 0x08000000u
#define CV_DTCM_START    0x20000000u

static uint32_t word(const uint8_t* bytes, uint32_t index) {
  uint32_t value;
  memcpy(&value, bytes + 4u * index, sizeof value);
  return value;
}

// The stack pointer a reset loads lies in DTCM, word aligned, and the reset handler is a Thumb
// address inside the carried bytes once they sit at sector 0.
static bool vectorsOk(const uint8_t* bytes, uint32_t length) {
  if (length < 8u) return false;
  const uint32_t stack = word(bytes, 0u);
  const uint32_t reset = word(bytes, 1u);
  const bool stackOk = stack > CV_DTCM_START && stack <= CV_DTCM_END && stack % 4u == 0u;
  const bool resetOk = (reset & 1u) != 0u && reset >= CV_SECTOR0_START + 8u && reset < CV_SECTOR0_START + length;
  return stackOk && resetOk;
}

CvPayloadState cvPayloadCheck(const struct CvPayloadHeader* header, const uint8_t* bytes, uint32_t available) {
  if (header->magic != CV_PAYLOAD_MAGIC) return CvPayloadNoMagic;
  if (header->length > CV_FLASH_SECTOR_SIZE) return CvPayloadTooLarge;
  if (header->length == 0u || header->length != available) return CvPayloadLengthWrong;
  if (cvCrc32(0u, bytes, header->length) != header->crc32) return CvPayloadCrcWrong;
  if (!vectorsOk(bytes, header->length)) return CvPayloadNotBootloader;
  return CvPayloadGood;
}

const char* cvPayloadStateName(CvPayloadState state) {
  switch (state) {
    case CvPayloadGood: return "good";
    case CvPayloadNoMagic: return "no magic";
    case CvPayloadTooLarge: return "too large";
    case CvPayloadLengthWrong: return "length wrong";
    case CvPayloadCrcWrong: return "crc wrong";
    case CvPayloadNotBootloader: return "not a bootloader";
    default: return "unknown";
  }
}

uint32_t cvSector0WritableWpsn(uint32_t wpsnPrg1) {
  return wpsnPrg1 | CV_WPSN_SECTOR0;
}
