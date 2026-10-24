// The bootloader's decision table as pure C99. See boot_decide.h.

#include "boot_decide.h"

// Bank 2 starts where the application region ends and holds the layout record and the diagnostic
// log. Its end is the 2 MB part's.
#define CV_BANK2_BASE  CV_APP_END
#define CV_BANK2_END   0x08200000u
CV_STATIC_ASSERT(diag_log_inside_bank2,
                 CV_DIAG_LOG_ADDR >= CV_BANK2_BASE && CV_DIAG_LOG_ADDR + CV_DIAG_LOG_SIZE <= CV_BANK2_END);

static bool inBank2(uint32_t address) {
  return address >= CV_BANK2_BASE && address < CV_BANK2_END;
}

// The 128 KB sector holding an application address, or 0 outside the application region. Sector 0
// is the bootloader and bank 2 holds the layout record, so neither is ever returned.
static uint32_t appSectorOf(uint32_t address) {
  if (address < CV_APP_START || address >= CV_APP_END) return 0u;
  return address & ~(CV_FLASH_SECTOR_SIZE - 1u);
}

static void clearSummary(struct CvBootBlock* block) {
  for (size_t i = 0; i < sizeof block->appSummary; ++i) block->appSummary[i] = '\0';
}

// A block with another version word was not written by this bootloader since power-on, so its
// diagnostic fields are garbage. The request and the counter validate themselves. The reason
// indexes name tables, so it is range checked on every call.
static void adoptBlock(struct CvBootBlock* block) {
  if (block->blockVersion != CV_BOOT_BLOCK_VERSION) {
    block->lastReason = (uint32_t)CvReasonNone;
    block->faultAddress = 0u;
    clearSummary(block);
    for (size_t i = 0; i < sizeof block->reserved; ++i) block->reserved[i] = 0u;
    block->blockVersion = CV_BOOT_BLOCK_VERSION;
  }
  if (block->lastReason > (uint32_t)CvReasonBootFailed) block->lastReason = (uint32_t)CvReasonNone;
}

CvBootDecision cvBootDecide(struct CvBootBlock* block, bool menuHeld, CvImageStatus image) {
  CvBootDecision decision = {CvActionUpdateMode, CvReasonNone, 0u};

  adoptBlock(block);
  const uint32_t fails = cvBootFailsGet(block);
  const bool requested = block->request == CV_REQUEST_UPDATE;
  const bool readFault = requested && block->lastReason == (uint32_t)CvReasonImageUnreadable;

  block->request = 0u;
  if (readFault) {
    decision.eraseAddress = appSectorOf(block->faultAddress);
  } else if (!inBank2(block->faultAddress)) {
    block->faultAddress = 0u;
  }

  if (menuHeld) {
    decision.reason = CvReasonMenuHeld;
  } else if (readFault) {
    decision.reason = decision.eraseAddress != 0u ? CvReasonImageUnreadable : CvReasonImageInvalid;
  } else if (requested) {
    decision.reason = CvReasonAppRequest;
  } else if (image != CvImageValid) {
    decision.reason = CvReasonImageInvalid;
  } else if (fails >= CV_BOOT_FAIL_LIMIT) {
    decision.reason = CvReasonBootFailed;
  } else {
    decision.action = CvActionRunApp;
  }

  if (decision.action == CvActionRunApp) {
    cvBootFailsSet(block, fails + 1u);
  } else {
    block->lastReason = (uint32_t)decision.reason;
  }
  return decision;
}

void cvBootRecordReadFault(struct CvBootBlock* block, uint32_t flashAddress) {
  adoptBlock(block);
  block->faultAddress = flashAddress;
  if (inBank2(flashAddress)) return;
  block->request = CV_REQUEST_UPDATE;
  block->lastReason = (uint32_t)CvReasonImageUnreadable;
}

bool cvBootTimeoutAllowed(const struct CvBootBlock* block, CvImageStatus image, bool hostEnumerated,
                          bool writeBegun) {
  const bool leavable = block->lastReason == (uint32_t)CvReasonMenuHeld ||
                        block->lastReason == (uint32_t)CvReasonAppRequest;
  return leavable && image == CvImageValid && !hostEnumerated && !writeBegun;
}

void cvBootMarkUpdated(struct CvBootBlock* block) {
  cvBootFailsSet(block, 0u);
  block->request = 0u;
  block->lastReason = (uint32_t)CvReasonNone;
  block->faultAddress = 0u;
  clearSummary(block);
  block->blockVersion = CV_BOOT_BLOCK_VERSION;
}
