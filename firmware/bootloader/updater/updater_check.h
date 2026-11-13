// The updater's checks on the bootloader it carries, pure C99 over memory pointers. The updater runs
// them on flash and on its RAM copy before it touches sector 0; tools/updatercheck.c runs the same
// code on files for build.sh.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "boot_block.h"

#ifdef __cplusplus
extern "C" {
#endif

// The bytes C V B L at the start of the payload header.
#define CV_PAYLOAD_MAGIC 0x4C425643u

// The 16 bytes in front of the carried bootloader. build.sh fills length and crc32 from the
// bootloader .bin; payload.S places the header and then the bytes, the last thing in the image.
struct CvPayloadHeader {
  // CV_PAYLOAD_MAGIC.
  uint32_t magic;
  // Bytes of the carried bootloader.
  uint32_t length;
  // CRC-32 (zlib polynomial) over those bytes.
  uint32_t crc32;
  // Zero.
  uint32_t reserved;
};

CV_STATIC_ASSERT(payload_header_16_bytes, sizeof(struct CvPayloadHeader) == 16u);

typedef enum {
  CvPayloadGood,
  CvPayloadNoMagic,
  // Over bank 1 sector 0.
  CvPayloadTooLarge,
  // Zero, or not the number of bytes the file carries.
  CvPayloadLengthWrong,
  CvPayloadCrcWrong,
  // The first two words are not a stack in DTCM and a reset handler inside the carried bytes.
  CvPayloadNotBootloader
} CvPayloadState;

// Checks the carried bootloader: `bytes` holds `available` bytes after `header`. In order: the magic,
// the length against sector 0 and against `available`, the CRC, then the vector table a bootloader
// in sector 0 starts with.
CvPayloadState cvPayloadCheck(const struct CvPayloadHeader* header, const uint8_t* bytes, uint32_t available);

// How the updater's summary and the host tool name a state.
const char* cvPayloadStateName(CvPayloadState state);

// WPSN_PRG1 with bank 1 sector 0 writable and every other sector as it stands.
uint32_t cvSector0WritableWpsn(uint32_t wpsnPrg1);

#ifdef __cplusplus
}
#endif
