// Bootloader and application contract: the addresses, magics and ids both programs rely on, the
// structs they share, and the names both programs print for the values in them. One definition for
// the C99 bootloader, the C++17 firmware and host tests, tools/stamp_image.py and the linker
// scripts.
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
// The first word of a valid fault record. As text it reads FAU3, most significant byte first.
#define CV_FAULT_MAGIC           0x46415533u
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

// How DIAG.TXT and the boot report spell a reason. A value past the enum reads "unknown".
static inline const char* cvBootReasonName(uint32_t reason) {
  switch (reason) {
    case CvReasonNone: return "none";
    case CvReasonMenuHeld: return "MenuHeld";
    case CvReasonAppRequest: return "AppRequest";
    case CvReasonImageInvalid: return "ImageInvalid";
    case CvReasonImageUnreadable: return "ImageUnreadable";
    case CvReasonBootFailed: return "BootFailed";
    default: return "unknown";
  }
}

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

// ---- Fault record --------------------------------------------------------------------------

// What stopped the application. The first four are the Cortex-M exceptions, numbered as the fault
// trampolines in board/fault.cpp pass them. Clock is a failed clock setup.
enum CvFaultKind {
  CvFaultKindHard = 0,
  CvFaultKindMemManage = 1,
  CvFaultKindBus = 2,
  CvFaultKindUsage = 3,
  CvFaultKindClock = 4
};

// The clock setup step that failed, stored in CvFaultRecord::detail for CvFaultKindClock.
enum CvClockStep {
  CvClockStepSupply = 1,
  CvClockStepVoltageScale = 2,
  CvClockStepOscillator = 3,
  CvClockStepClockTree = 4,
  CvClockStepPeripheralClocks = 5
};

// The record at CV_FAULT_RECORD_ADDR. The application's fault handler fills the fields, writes the
// magic last and resets. The application reads the record at the next boot and clears the magic.
// The bootloader reads it for DIAG.TXT and never clears it. Readers check the magic first.
struct CvFaultRecord {
  // CV_FAULT_MAGIC when the record is valid.
  uint32_t magic;
  // An enum CvFaultKind.
  uint32_t kind;
  // The stacked pc and lr, 0 when the exception frame was unreadable.
  uint32_t pc;
  uint32_t lr;
  // SCB->CFSR, HFSR, BFAR and MMFAR at the fault.
  uint32_t cfsr;
  uint32_t hfsr;
  uint32_t bfar;
  uint32_t mmfar;
  // FLASH->SR2 and FLASH->ECC_FA2 at the fault, for a double ECC error in bank 2.
  uint32_t sr2;
  uint32_t eccFa2;
  // An enum CvClockStep for CvFaultKindClock, else 0.
  uint32_t detail;
};

CV_STATIC_ASSERT(fault_record_fits_its_slot, sizeof(struct CvFaultRecord) <= CV_FAULT_RECORD_SIZE);
CV_STATIC_ASSERT(fault_magic_at_00, offsetof(struct CvFaultRecord, magic) == 0x00u);

// How faultReport and DIAG.TXT spell a kind. A value past the enum reads "unknown".
static inline const char* cvFaultKindName(uint32_t kind) {
  switch (kind) {
    case CvFaultKindHard: return "hard";
    case CvFaultKindMemManage: return "memmanage";
    case CvFaultKindBus: return "bus";
    case CvFaultKindUsage: return "usage";
    case CvFaultKindClock: return "clock";
    default: return "unknown";
  }
}

// How a clock failure names its step. Zero and any value past the enum read "?".
static inline const char* cvClockStepName(uint32_t step) {
  switch (step) {
    case CvClockStepSupply: return "supply";
    case CvClockStepVoltageScale: return "voltage scaling";
    case CvClockStepOscillator: return "oscillator and PLL1";
    case CvClockStepClockTree: return "bus clocks";
    case CvClockStepPeripheralClocks: return "peripheral clocks";
    default: return "?";
  }
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

// ---- Diagnostic log ------------------------------------------------------------------------

// Bank 2 sector 2. The application appends fixed-size entries and erases the sector when it is
// full; the bootloader lists the newest entries in DIAG.TXT and never writes here. An entry is
// blank when every byte is 0xFF, valid when its magic matches and the trailing crc32 covers the
// preceding bytes, torn otherwise.
#define CV_DIAG_LOG_ADDR     0x08140000u
#define CV_DIAG_LOG_SIZE     0x20000u
#define CV_DIAG_ENTRY_SIZE   256u
// The bytes C V D G.
#define CV_DIAG_MAGIC        0x47445643u
// A numeric field nobody knew when the entry was written.
#define CV_DIAG_UNKNOWN_U32  0xFFFFFFFFu

// What the application was doing when it wrote the entry.
enum CvDiagKind {
  CvDiagKindBoot = 1,
  CvDiagKindUpdateRequest = 2
};

// One log entry. Text fields are NUL padded. Reserved bytes stay 0xFF so a later field can be
// added without a new magic.
struct CvDiagEntry {
  // CV_DIAG_MAGIC.
  uint32_t magic;
  // Counts up across erases, never CV_DIAG_UNKNOWN_U32.
  uint32_t sequence;
  // An enum CvDiagKind.
  uint32_t kind;
  // Seconds since the application started, or CV_DIAG_UNKNOWN_U32 for a boot entry.
  uint32_t uptimeS;
  // RCC reset flags as read at this boot.
  uint32_t resetCause;
  // The bootloader's boot-fail count as the application saw it at this boot.
  uint32_t bootFails;
  // CvBootBlock::lastReason and faultAddress as the bootloader left them.
  uint32_t lastReason;
  uint32_t faultAddress;
  // Audio error counters of the run that wrote the entry.
  uint32_t saiErrors;
  uint32_t overruns;
  // Release name, the CUBEVOX_FW_VERSION string.
  char firmware[32];
  // Fault record summary as faultSummaryText prints it, or empty.
  char fault[64];
  // The previous run's CvBootBlock::appSummary, or empty.
  char summary[64];
  uint8_t reserved[52];
  // CRC-32 (zlib polynomial) over the preceding 252 bytes.
  uint32_t crc32;
};

CV_STATIC_ASSERT(diag_entry_256_bytes, sizeof(struct CvDiagEntry) == CV_DIAG_ENTRY_SIZE);
CV_STATIC_ASSERT(diag_crc_is_last, offsetof(struct CvDiagEntry, crc32) == CV_DIAG_ENTRY_SIZE - 4u);
CV_STATIC_ASSERT(diag_log_in_bank_2,
                 CV_DIAG_LOG_ADDR >= 0x08100000u && CV_DIAG_LOG_ADDR + CV_DIAG_LOG_SIZE <= 0x08200000u);
CV_STATIC_ASSERT(diag_log_one_sector,
                 CV_DIAG_LOG_SIZE == CV_FLASH_SECTOR_SIZE && CV_DIAG_LOG_ADDR % CV_FLASH_SECTOR_SIZE == 0u);
CV_STATIC_ASSERT(diag_entries_fill_sector, CV_DIAG_LOG_SIZE % CV_DIAG_ENTRY_SIZE == 0u);

// ---- Flash map checks ----------------------------------------------------------------------

CV_STATIC_ASSERT(app_region_on_sector_bounds,
                 CV_APP_START < CV_APP_END && CV_APP_START % CV_FLASH_SECTOR_SIZE == 0u &&
                     CV_APP_END % CV_FLASH_SECTOR_SIZE == 0u);
// 0x298 bytes is the vector table of the H743 (166 vectors).
CV_STATIC_ASSERT(header_after_vector_table,
                 CV_IMAGE_HEADER_OFFSET >= 0x298u && CV_IMAGE_HEADER_OFFSET % 4u == 0u);
CV_STATIC_ASSERT(header_in_first_app_sector,
                 CV_IMAGE_HEADER_OFFSET + CV_IMAGE_HEADER_SIZE <= CV_FLASH_SECTOR_SIZE);
