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
// FAIL_ECC_ADDR in 32-byte flash words, the bank 2 base. board_hooks.c checks them against CMSIS.
#define CV_FLASH_SR_DBECCERR  (1u << 26)
#define CV_FLASH_ECC_ADDR     0x7FFFu
#define CV_FLASH_WORD_BYTES   32u
#define CV_FLASH_BANK2_BASE   0x08100000u

typedef struct {
  const struct CvBootBlock* block;
  const struct CvImageReport* image;
  const struct CvFaultRecord* fault;
  // The USB serial: 24 uppercase hex characters, the UID bytes in address order.
  const char* serial;
  const char* tinyusbVersion;
  // UF2 blocks refused since update mode began, and the family id of the last one.
  uint32_t refusedBlocks;
  uint32_t lastRefusedFamily;
  // The diagnostic log sector, CV_DIAG_LOG_SIZE bytes. On the box it is the flash at
  // CV_DIAG_LOG_ADDR. Never read when block->faultAddress lies inside it.
  const uint8_t* log;
} CvDriveState;

// The installed release name, "none" without an image, or "invalid".
const char* cvInstalledText(const struct CvImageReport* image);

// INFO_UF2.TXT. Returns the text and sets *length.
const char* cvInfoText(const CvDriveState* state, uint32_t* length);

// Writes the `flash log:` lines of DIAG.TXT to out[0..capacity), NUL terminated, and returns their
// length. The newest 16 valid entries come first, one line each, then the counts of valid, torn and
// blank slots. A slot is blank when every byte is 0xFF, valid when its magic and trailing CRC-32
// hold, torn otherwise. Only whole lines are written; a line that does not fit is left out with every
// older one. Without a valid entry the text is `flash log: empty`.
//
// A block->faultAddress inside the log sector says a read there faulted. The text is then
// `flash log: unreadable at <address>` and `sector` is not touched. capacity is at least 1.
uint32_t cvDiagLogLines(const struct CvBootBlock* block, const uint8_t* sector, char* out, uint32_t capacity);

// Rewrites DIAG.TXT in cvDiagText (board.h), padded with spaces to its fixed size.
void cvDiagWrite(const CvDriveState* state);

#ifdef __cplusplus
}
#endif
