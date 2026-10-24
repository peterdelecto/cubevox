// Bootloader and application contract: the addresses, magics and ids both programs rely on, and
// the two structs they share. One definition for the C99 bootloader, the C++17 firmware and host
// tests, tools/stamp_image.py and the linker scripts.
//
// tools/stamp_image.py, firmware/build.sh and the linker scripts read the #define lines by
// pattern. A value they read stays an integer literal alone on its own #define line.
//
// Offsets, magics, ids and the application start are frozen once a box ships. New fields take
// reserved space and bump the version word they belong to.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Compile-time check for C99, C11 and C++. The name labels the failure.
#if defined(__cplusplus)
#define CV_STATIC_ASSERT(name, cond) static_assert(cond, #name)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define CV_STATIC_ASSERT(name, cond) _Static_assert(cond, #name)
#else
#define CV_STATIC_ASSERT(name, cond) typedef char cvAssert_##name[(cond) ? 1 : -1]
#endif

// ---- DTCM ----------------------------------------------------------------------------------

// The top 256 bytes of DTCM are shared by both programs. struct CvBootBlock takes the first 192
// and the fault record (.cv_noinit) the last 64. A system reset keeps them and power-on leaves
// them random.
#define CV_DTCM_END           0x20020000u
#define CV_BOOT_BLOCK_ADDR    0x2001FF00u
#define CV_BOOT_BLOCK_SIZE    256u
#define CV_FAULT_RECORD_ADDR  0x2001FFC0u
#define CV_FAULT_RECORD_SIZE  64u

// ---- Flash ---------------------------------------------------------------------------------

// The bootloader owns bank 1 sector 0. The application owns the rest of bank 1 and carries its
// header at +0x400, after the vector table. Bank 2 holds the layout record and the diagnostic log.
#define CV_FLASH_SECTOR_SIZE    0x20000u
#define CV_APP_START            0x08020000u
#define CV_APP_END              0x08100000u
#define CV_IMAGE_HEADER_OFFSET  0x400u

// ---- Image header layout -------------------------------------------------------------------

#define CV_IMAGE_HEADER_SIZE            32u
#define CV_IMAGE_OFFSET_MAGIC           0u
#define CV_IMAGE_OFFSET_HEADER_VERSION  4u
#define CV_IMAGE_OFFSET_LENGTH          8u
#define CV_IMAGE_OFFSET_CRC             12u
#define CV_IMAGE_OFFSET_VERSION         16u
#define CV_IMAGE_VERSION_BYTES          16u

// ---- Magics and versions -------------------------------------------------------------------

// TinyUF2's double-tap word. The application writes it to CvBootBlock::request, then resets.
#define CV_REQUEST_UPDATE        0xF01669EFu
// The bytes C V F W at the start of the image header.
#define CV_IMAGE_MAGIC           0x57465643u
#define CV_IMAGE_HEADER_VERSION  1u
#define CV_BOOT_BLOCK_VERSION    1u

// ---- UF2 families --------------------------------------------------------------------------

// cubevox-private ids ("CVBX" and "CVWA" in ASCII), not registered in microsoft/uf2
// uf2families.json. Each bootloader accepts only its own family, so a bench image for the WeAct
// board or another project's file never reaches the cubevox flash.
#define CV_UF2_FAMILY_CUBEVOX  0x43564258u
#define CV_UF2_FAMILY_WEACT    0x43565741u

// ---- USB identity --------------------------------------------------------------------------

// The bootloader enumerates as the pid.codes test id and the application keeps its CDC id. They
// differ because macOS and Windows remember the device class per id.
#define CV_USB_VID_BOOT  0x1209u
#define CV_USB_PID_BOOT  0x0001u
#define CV_USB_VID_APP   0x0483u
#define CV_USB_PID_APP   0x5740u

// ---- Limits --------------------------------------------------------------------------------

// Starts without a healthy report before the bootloader stays in update mode.
#define CV_BOOT_FAIL_LIMIT    3u
// Update mode falls back to the application after this long when no host has enumerated.
#define CV_UPDATE_TIMEOUT_MS  60000u
// Audio blocks must advance this long after setup() before the application reports healthy.
#define CV_HEALTHY_MS         5000u

// ---- Boot block ----------------------------------------------------------------------------

// Why the bootloader last stayed in update mode.
enum CvBootReason {
  CvReasonNone = 0,
  CvReasonMenuHeld,
  CvReasonAppRequest,
  CvReasonImageInvalid,
  CvReasonImageUnreadable,
  CvReasonBootFailed
};

// The 192 bytes at CV_BOOT_BLOCK_ADDR. Readers validate every field before use. The reason is a
// uint32_t so the layout does not depend on the compiler's enum width.
struct CvBootBlock {
  // CV_REQUEST_UPDATE asks the bootloader to stay in update mode.
  uint32_t request;
  // Starts since the last healthy report.
  uint32_t bootFails;
  // ~bootFails. A mismatch is power-on garbage and reads as 0.
  uint32_t bootFailsCheck;
  // CV_BOOT_BLOCK_VERSION, written by the bootloader.
  uint32_t blockVersion;
  // An enum CvBootReason.
  uint32_t lastReason;
  // Flash address of the last read fault, else 0.
  uint32_t faultAddress;
  // Text the application writes before it requests update mode, NUL padded.
  char appSummary[40];
  // Zero when written.
  uint8_t reserved[128];
};

CV_STATIC_ASSERT(boot_block_192_bytes, sizeof(struct CvBootBlock) == 0xC0u);
CV_STATIC_ASSERT(boot_block_request_at_00, offsetof(struct CvBootBlock, request) == 0x00u);
CV_STATIC_ASSERT(boot_block_fails_at_04, offsetof(struct CvBootBlock, bootFails) == 0x04u);
CV_STATIC_ASSERT(boot_block_check_at_08, offsetof(struct CvBootBlock, bootFailsCheck) == 0x08u);
CV_STATIC_ASSERT(boot_block_version_at_0c, offsetof(struct CvBootBlock, blockVersion) == 0x0Cu);
CV_STATIC_ASSERT(boot_block_reason_at_10, offsetof(struct CvBootBlock, lastReason) == 0x10u);
CV_STATIC_ASSERT(boot_block_fault_at_14, offsetof(struct CvBootBlock, faultAddress) == 0x14u);
CV_STATIC_ASSERT(boot_block_summary_at_18, offsetof(struct CvBootBlock, appSummary) == 0x18u);
CV_STATIC_ASSERT(boot_block_reserved_at_40, offsetof(struct CvBootBlock, reserved) == 0x40u);
CV_STATIC_ASSERT(boot_block_ends_at_fault_record,
                 CV_BOOT_BLOCK_ADDR + sizeof(struct CvBootBlock) == CV_FAULT_RECORD_ADDR);
CV_STATIC_ASSERT(boot_region_ends_at_dtcm_end, CV_BOOT_BLOCK_ADDR + CV_BOOT_BLOCK_SIZE == CV_DTCM_END);
CV_STATIC_ASSERT(fault_record_ends_at_dtcm_end,
                 CV_FAULT_RECORD_ADDR + CV_FAULT_RECORD_SIZE == CV_DTCM_END);

// True when the check word agrees with the counter.
static inline bool cvBootFailsValid(const struct CvBootBlock* block) {
  return block->bootFailsCheck == (uint32_t)~block->bootFails;
}

// The counter, or 0 when the check word disagrees.
static inline uint32_t cvBootFailsGet(const struct CvBootBlock* block) {
  return cvBootFailsValid(block) ? block->bootFails : 0u;
}

// Writes both words so a later reader sees an agreeing pair.
static inline void cvBootFailsSet(struct CvBootBlock* block, uint32_t count) {
  block->bootFails = count;
  block->bootFailsCheck = (uint32_t)~count;
}

// ---- Image header --------------------------------------------------------------------------

// The 32-byte header at CV_APP_START + CV_IMAGE_HEADER_OFFSET. The compiler emits length and
// crc32 as zero and tools/stamp_image.py fills them after the link.
struct CvImageHeader {
  // CV_IMAGE_MAGIC.
  uint32_t magic;
  // CV_IMAGE_HEADER_VERSION.
  uint32_t headerVersion;
  // Bytes in the whole image file.
  uint32_t length;
  // CRC-32 (zlib polynomial) over the whole image with this field as zero.
  uint32_t crc32;
  // Release name such as v2.00.10 or v2.00.10-bench, NUL padded.
  char version[16];
};

CV_STATIC_ASSERT(image_header_32_bytes, sizeof(struct CvImageHeader) == CV_IMAGE_HEADER_SIZE);
CV_STATIC_ASSERT(image_magic_at_0,
                 offsetof(struct CvImageHeader, magic) == CV_IMAGE_OFFSET_MAGIC);
CV_STATIC_ASSERT(image_header_version_at_4,
                 offsetof(struct CvImageHeader, headerVersion) == CV_IMAGE_OFFSET_HEADER_VERSION);
CV_STATIC_ASSERT(image_length_at_8,
                 offsetof(struct CvImageHeader, length) == CV_IMAGE_OFFSET_LENGTH);
CV_STATIC_ASSERT(image_crc_at_12, offsetof(struct CvImageHeader, crc32) == CV_IMAGE_OFFSET_CRC);
CV_STATIC_ASSERT(image_version_at_16,
                 offsetof(struct CvImageHeader, version) == CV_IMAGE_OFFSET_VERSION);
CV_STATIC_ASSERT(image_version_fills_header,
                 CV_IMAGE_OFFSET_VERSION + CV_IMAGE_VERSION_BYTES == CV_IMAGE_HEADER_SIZE);

// ---- Flash map checks ----------------------------------------------------------------------

CV_STATIC_ASSERT(app_region_on_sector_bounds,
                 CV_APP_START < CV_APP_END && CV_APP_START % CV_FLASH_SECTOR_SIZE == 0u &&
                     CV_APP_END % CV_FLASH_SECTOR_SIZE == 0u);
// 0x298 bytes is the vector table of the H743 (166 vectors).
CV_STATIC_ASSERT(header_after_vector_table,
                 CV_IMAGE_HEADER_OFFSET >= 0x298u && CV_IMAGE_HEADER_OFFSET % 4u == 0u);
CV_STATIC_ASSERT(header_in_first_app_sector,
                 CV_IMAGE_HEADER_OFFSET + CV_IMAGE_HEADER_SIZE <= CV_FLASH_SECTOR_SIZE);
