// The text files of the update drive, INFO_UF2.TXT and DIAG.TXT, built without printf and without
// HAL, so tools/drivetext.c renders the same text on the host. board_hooks.c gathers the inputs.

#pragma once

#include <stdint.h>

#include "boot_block.h"
#include "image_check.h"

#ifdef __cplusplus
extern "C" {
#endif

// RM0433 register fields the fault line decodes as fault.cpp does: FLASH_SR DBECCERR, FLASH_ECC_FA
// FAIL_ECC_ADDR in 32-byte flash words. board_hooks.c checks them against CMSIS.
#define CV_FLASH_SR_DBECCERR  (1u << 26)
#define CV_FLASH_ECC_ADDR     0x7FFFu
#define CV_FLASH_WORD_BYTES   32u

// UF2 blocks since update mode began.
typedef struct {
  uint32_t written;
  // Refused by the filter, and the family id of the last one.
  uint32_t refused;
  uint32_t lastRefusedFamily;
  // Accepted but not written: the erase or the program failed. The address of the first one.
  uint32_t failed;
  uint32_t firstFailedAddress;
} CvBlockCounts;

typedef struct {
  const struct CvBootBlock* block;
  const struct CvImageReport* image;
  const struct CvFaultRecord* fault;
  // The USB serial: 24 uppercase hex characters, the UID bytes in address order.
  const char* serial;
  const char* tinyusbVersion;
  CvBlockCounts blocks;
  // The 8.3 name of a file the host copied that cannot be firmware, "" when none is known.
  const char* wrongFile;
  // The CvClockStep that failed at this start, 0 when the crystal runs.
  uint32_t clockStep;
  // FLASH OPTSR_CUR and WPSN_CUR1 as read at this start.
  uint32_t optsrCur;
  uint32_t wpsnCur1;
  // The diagnostic log sector, CV_DIAG_LOG_SIZE bytes. On the box it is the flash at
  // CV_DIAG_LOG_ADDR. Never read when block->faultAddress lies in bank 2.
  const uint8_t* log;
} CvDriveState;

// The installed release name, "none" without an image, or "invalid".
const char* cvInstalledText(const struct CvImageReport* image);

// Rewrites INFO_UF2.TXT, padded with spaces to CV_INFO_TEXT_BYTES so the directory never changes
// when the text does, and returns it.
const char* cvInfoText(const CvDriveState* state);

// Writes the `flash log:` lines of DIAG.TXT to out[0..capacity), NUL terminated, and returns their
// length. The newest 16 valid entries come first, one line each, then the counts of valid, torn and
// blank slots. A slot is blank when every byte is 0xFF, valid when its magic and trailing CRC-32
// hold, torn otherwise. Only whole lines are written; a line that does not fit is left out with every
// older one. Without a valid entry the text is `flash log: empty`.
//
// A block->faultAddress in bank 2 says a read of the log faulted, since the bootloader reads nothing
// else there. The text is then `flash log: unreadable at <address>` and `sector` is not touched.
// capacity is at least 1.
uint32_t cvDiagLogLines(const struct CvBootBlock* block, const uint8_t* sector, char* out, uint32_t capacity);

// Rewrites DIAG.TXT in cvDiagText (board.h), padded with spaces to its fixed size.
void cvDiagWrite(const CvDriveState* state);

#ifdef __cplusplus
}
#endif
