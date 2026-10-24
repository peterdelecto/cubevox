// The bootloader's policy as pure C99. See boot_decide.h.

#include "boot_decide.h"

#include <string.h>

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
  memset(block->appSummary, 0, sizeof block->appSummary);
}

static void clearBootFault(struct CvBootBlock* block) {
  block->bootFaultPc = 0u;
  block->bootFaultCfsr = 0u;
  block->bootFaultHfsr = 0u;
  block->bootFaultBfar = 0u;
}

bool cvBootPowerOn(uint32_t resetFlags) {
  const uint32_t powered = CV_RSR_PORRSTF | CV_RSR_BORRSTF;
  const uint32_t restarted = CV_RSR_SFTRSTF | CV_RSR_IWDG1RSTF | CV_RSR_WWDG1RSTF;
  return (resetFlags & powered) != 0u && (resetFlags & restarted) == 0u;
}

void cvBootAdopt(struct CvBootBlock* block, uint32_t resetFlags) {
  if (cvBootPowerOn(resetFlags) || !cvBootBlockInitialised(block)) {
    memset(block, 0, sizeof *block);
    cvBootFailsSet(block, 0u);
  }
  if (!cvBootReasonKnown(block->lastReason)) block->lastReason = (uint32_t)CvReasonNone;
  block->blockVersion = CV_BOOT_BLOCK_VERSION;
}

bool cvBootRequestPending(const struct CvBootBlock* block) {
  return block->request == CV_REQUEST_UPDATE || block->request == CV_REQUEST_RESUME;
}

// The reason a resume request returns to. Update mode always stores one, so anything else is a
// fault the bootloader could not name.
static enum CvBootReason resumeReason(uint32_t lastReason) {
  if (lastReason == (uint32_t)CvReasonNone || !cvBootReasonKnown(lastReason)) return CvReasonBootFault;
  return (enum CvBootReason)lastReason;
}

static enum CvBootReason reasonFor(const struct CvBootBlock* block, const CvBootInputs* in,
                                   const CvBootDecision* decision, bool readFault) {
  const uint32_t fails = cvBootFailsGet(block);
  if (in->clockStep != 0u) return CvReasonClockFailed;
  if (in->menuHeld) return CvReasonMenuHeld;
  if (readFault) return decision->eraseAddress != 0u ? CvReasonImageUnreadable : CvReasonImageInvalid;
  if (block->request == CV_REQUEST_RESUME) return resumeReason(block->lastReason);
  if (block->request == CV_REQUEST_UPDATE) return CvReasonAppRequest;
  if (in->image != CvImageValid) return CvReasonImageInvalid;
  if (fails >= CV_BOOT_FAIL_LIMIT) return CvReasonBootFailed;
  return CvReasonNone;
}

CvBootDecision cvBootDecide(struct CvBootBlock* block, const CvBootInputs* in) {
  CvBootDecision decision = {CvActionUpdateMode, CvReasonNone, 0u, true};
  const bool readFault =
      block->request == CV_REQUEST_RESUME && block->lastReason == (uint32_t)CvReasonImageUnreadable;
  if (readFault) {
    decision.eraseAddress = appSectorOf(block->faultAddress);
    decision.imageReadable = decision.eraseAddress != 0u;
  } else if (!inBank2(block->faultAddress)) {
    block->faultAddress = 0u;
  }

  decision.reason = reasonFor(block, in, &decision, readFault);
  block->request = 0u;
  if (decision.reason == CvReasonNone) {
    decision.action = CvActionRunApp;
    cvBootFailsSet(block, cvBootFailsGet(block) + 1u);
  } else {
    block->lastReason = (uint32_t)decision.reason;
  }
  return decision;
}

void cvBootRecordFault(struct CvBootBlock* block, const CvFaultInfo* fault) {
  if (!fault->blockAdopted) cvBootAdopt(block, fault->resetFlags);
  block->bootFaultPc = fault->pc;
  block->bootFaultCfsr = fault->cfsr;
  block->bootFaultHfsr = fault->hfsr;
  block->bootFaultBfar = fault->bfar;
  if (inBank2(fault->eccAddress)) {
    block->faultAddress = fault->eccAddress;
    if (fault->updateMode) block->request = CV_REQUEST_RESUME;
    return;
  }
  if (fault->eccAddress != 0u || fault->imageReading) {
    block->request = CV_REQUEST_RESUME;
    block->faultAddress = fault->eccAddress;
    block->lastReason = (uint32_t)CvReasonImageUnreadable;
    return;
  }
  // A second fault in an update mode the first one started would repeat at every start, so the
  // next start decides afresh and a valid image runs.
  const bool again = fault->updateMode && block->lastReason == (uint32_t)CvReasonBootFault;
  block->request = again ? 0u : CV_REQUEST_RESUME;
  block->lastReason = (uint32_t)CvReasonBootFault;
}

bool cvBootTimeoutAllowed(const struct CvBootBlock* block, CvImageStatus image, bool hostSeen,
                          bool writeBegun) {
  const uint32_t reason = block->lastReason;
  const bool leavable = reason == (uint32_t)CvReasonMenuHeld || reason == (uint32_t)CvReasonAppRequest ||
                        reason == (uint32_t)CvReasonBootFault;
  return leavable && image == CvImageValid && !hostSeen && !writeBegun;
}

void cvBootMarkUpdated(struct CvBootBlock* block) {
  cvBootFailsSet(block, 0u);
  block->request = 0u;
  block->lastReason = (uint32_t)CvReasonNone;
  block->faultAddress = 0u;
  clearSummary(block);
  clearBootFault(block);
  block->blockVersion = CV_BOOT_BLOCK_VERSION;
}

// ---- Option bytes -------------------------------------------------------------------------

bool cvOptionsWanted(uint32_t optsrCur, uint32_t wpsnCur1) {
  const uint32_t borLevel = (optsrCur & CV_OPTSR_BOR_LEV_MASK) >> CV_OPTSR_BOR_LEV_POS;
  return borLevel != CV_BOR_LEVEL_WANTED || (wpsnCur1 & CV_WPSN_SECTOR0) != 0u;
}

bool cvOptionsAttempt(const struct CvBootBlock* block, uint32_t optsrCur, uint32_t wpsnCur1) {
  return !cvOptionResultFailed(block->optionBytes) && cvOptionsWanted(optsrCur, wpsnCur1);
}

uint32_t cvOptionsOptsr(uint32_t optsrPrg) {
  return (optsrPrg & ~CV_OPTSR_BOR_LEV_MASK) | (CV_BOR_LEVEL_WANTED << CV_OPTSR_BOR_LEV_POS);
}

uint32_t cvOptionsWpsn(uint32_t wpsnPrg1) {
  return wpsnPrg1 & ~CV_WPSN_SECTOR0;
}

// ---- Update mode --------------------------------------------------------------------------

// The flash word, 256 bits, is the smallest unit the H7 programs.
#define CV_FLASH_WORD_BYTES 32u

bool cvUf2BlockAccepted(uint32_t familyId, uint32_t address, uint32_t length) {
  const bool inRegion = address >= CV_APP_START && address < CV_APP_END && length <= CV_APP_END - address;
  const bool wholeWords =
      length != 0u && address % CV_FLASH_WORD_BYTES == 0u && length % CV_FLASH_WORD_BYTES == 0u;
  return familyId == CV_UF2_FAMILY_CUBEVOX && inRegion && wholeWords;
}

bool cvUf2SessionRestart(const CvUf2SessionInputs* in) {
  if (in->sessionBlocks == 0u) return false;
  const bool otherFile = in->blockNumBlocks != in->sessionBlocks || (in->sectorErased && in->targetHoldsOtherData);
  const bool retry = in->sessionFailed && in->blockNo == 0u;
  return otherFile || retry;
}

// FAT short directory entries (Microsoft FAT specification, section 6).
#define CV_DIR_ENTRY_BYTES  32u
#define CV_DIR_SECTOR_BYTES 512u
#define CV_DIR_ATTR_VOLUME  0x08u
#define CV_DIR_ATTR_DIR     0x10u
#define CV_DIR_ATTR_LFN     0x0Fu
#define CV_DIR_FREE         0x00u
#define CV_DIR_DELETED      0xE5u

// Extensions of the files a person might drag instead of the release .uf2: the raw image, build
// outputs and archives such as the release page's source code.
static const char kWrongExtensions[][4] = {"BIN", "HEX", "ELF", "DFU", "ZIP", "GZ ", "TGZ", "TAR", "7Z "};

static bool wrongExtension(const uint8_t* ext) {
  for (size_t i = 0; i < sizeof kWrongExtensions / sizeof kWrongExtensions[0]; ++i) {
    if (memcmp(ext, kWrongExtensions[i], 3u) == 0) return true;
  }
  return false;
}

// NAME.EXT from the padded 8 + 3 bytes, a byte outside printable ASCII as '?'.
static void shortName(const uint8_t* entry, char name[13]) {
  uint32_t n = 0u;
  for (uint32_t i = 0u; i < 11u; ++i) {
    const uint8_t c = entry[i];
    if (i == 8u && entry[8] != ' ') name[n++] = '.';
    if (c == ' ') continue;
    name[n++] = (c >= 0x20u && c < 0x7Fu) ? (char)c : '?';
  }
  name[n] = '\0';
}

bool cvDirNamesWrongFile(const uint8_t* sector, char name[13]) {
  for (uint32_t at = 0u; at < CV_DIR_SECTOR_BYTES; at += CV_DIR_ENTRY_BYTES) {
    const uint8_t* entry = &sector[at];
    if (entry[0] == CV_DIR_FREE) break;
    const uint8_t attr = entry[11];
    if (entry[0] == CV_DIR_DELETED || entry[0] == '.' || attr == CV_DIR_ATTR_LFN) continue;
    if ((attr & (CV_DIR_ATTR_VOLUME | CV_DIR_ATTR_DIR)) != 0u || !wrongExtension(&entry[8])) continue;
    shortName(entry, name);
    return true;
  }
  return false;
}

static CvScreen waitingScreen(const CvScreenInputs* in) {
  switch (in->reason) {
    case CvReasonMenuHeld:
    case CvReasonAppRequest:
    case CvReasonBootFault: return in->imageValid ? CvScreenWaitMenu : CvScreenWaitNoImage;
    case CvReasonBootFailed: return CvScreenWaitBootFailed;
    default: return CvScreenWaitNoImage;
  }
}

CvScreen cvUpdateScreen(const CvScreenInputs* in) {
  if (in->sessionFailed) return CvScreenWaitNoImage;
  if (in->sessionAccepted) return CvScreenWriting;
  if (in->sessionRefused) return CvScreenWrongFile;
  if (in->reason == (uint32_t)CvReasonClockFailed) return CvScreenClockFailed;
  if (in->cableOut && !in->timeoutArmed) return CvScreenCableOut;
  if (in->writeBegun) return CvScreenWaitNoImage;
  return waitingScreen(in);
}

void cvDebounceReset(CvDebounce* debounce, bool level) {
  debounce->level = level;
  debounce->pending = false;
  debounce->sinceMs = 0u;
}

bool cvDebounceUpdate(CvDebounce* debounce, bool sample, uint32_t nowMs, uint32_t holdMs) {
  if (sample == debounce->level) {
    debounce->pending = false;
    return false;
  }
  if (!debounce->pending) {
    debounce->pending = true;
    debounce->sinceMs = nowMs;
  }
  if (nowMs - debounce->sinceMs < holdMs) return false;
  debounce->level = sample;
  debounce->pending = false;
  return true;
}
