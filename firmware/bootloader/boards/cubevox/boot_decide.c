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
  if (cvBootPowerOn(resetFlags) || !cvBootBlockSealed(block)) {
    memset(block, 0, sizeof *block);
    cvBootFailsSet(block, 0u);
  }
  if (!cvBootReasonKnown(block->lastReason)) block->lastReason = (uint32_t)CvReasonNone;
  cvBootBlockSeal(block);
  block->resetFlags = resetFlags;
}

bool cvBootRequestPending(const struct CvBootBlock* block) {
  return block->request == CV_REQUEST_UPDATE || block->request == CV_REQUEST_RESUME;
}

// The reason a resume request returns to. Update mode always stores one, so anything else is a
// fault the bootloader could not name.
static enum CvBootReason resumeReason(uint32_t lastReason) {
  if (lastReason == (uint32_t)CvReasonNone || lastReason == (uint32_t)CvReasonMenuHeld ||
      !cvBootReasonKnown(lastReason)) {
    return CvReasonBootFault;
  }
  return (enum CvBootReason)lastReason;
}

static enum CvBootReason reasonFor(const struct CvBootBlock* block, const CvBootInputs* in,
                                   const CvBootDecision* decision, bool readFault) {
  const uint32_t fails = cvBootFailsGet(block);
  if (in->clockStep != 0u) return CvReasonClockFailed;
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
  const bool leavable = reason == (uint32_t)CvReasonAppRequest || reason == (uint32_t)CvReasonBootFault;
  return leavable && image == CvImageValid && !hostSeen && !writeBegun;
}

void cvBootMarkUpdated(struct CvBootBlock* block) {
  cvBootFailsSet(block, 0u);
  block->request = 0u;
  block->lastReason = (uint32_t)CvReasonNone;
  if (!inBank2(block->faultAddress)) block->faultAddress = 0u;
  clearSummary(block);
  clearBootFault(block);
  cvBootUpdatedWordsPut(block, 0u, 0u);
  cvBootBlockSeal(block);
}

static uint32_t reservedWord(const struct CvBootBlock* block, uint32_t index) {
  uint32_t word;
  memcpy(&word, &block->bootReserved[4u * index], sizeof word);
  return word;
}

bool cvBootUpdatedMarked(const struct CvBootBlock* block) {
  return reservedWord(block, 0u) == CV_BOOT_UPDATED_MARK && reservedWord(block, 1u) == ~CV_BOOT_UPDATED_MARK;
}

// ---- Option bytes -------------------------------------------------------------------------

bool cvOptionsWanted(uint32_t optsrCur, uint32_t wpsnCur1) {
  return !cvOptionsInPlace(optsrCur, wpsnCur1);
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

bool cvUf2BlockAccepted(uint32_t familyId, uint32_t address, uint32_t length, uint32_t blockNo,
                        uint32_t numBlocks) {
  const bool inRegion = address >= CV_APP_START && address < CV_APP_END && length <= CV_APP_END - address;
  const bool wholeWords =
      length != 0u && address % CV_FLASH_WORD_BYTES == 0u && length % CV_FLASH_WORD_BYTES == 0u;
  if (familyId != CV_UF2_FAMILY_CUBEVOX || !inRegion || !wholeWords || blockNo >= numBlocks) return false;
  const uint64_t fileBytes = (uint64_t)numBlocks * length;
  const bool basedAtStart = (uint64_t)blockNo * length == (uint64_t)(address - CV_APP_START);
  return basedAtStart && fileBytes <= CV_APP_END - CV_APP_START;
}

bool cvUpdateStalled(bool sessionAccepted, bool updateWritten, uint32_t nowMs, uint32_t lastBlockMs) {
  return sessionAccepted && !updateWritten && nowMs - lastBlockMs >= CV_WRITE_IDLE_MS;
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

// With no image, a bootloader the updater just wrote says so instead of "The update stopped.".
static CvScreen noImageScreen(const CvScreenInputs* in) {
  return in->bootUpdated && !in->imageValid ? CvScreenBootUpdated : CvScreenWaitNoImage;
}

static CvScreen waitingScreen(const CvScreenInputs* in) {
  switch (in->reason) {
    case CvReasonAppRequest:
    case CvReasonBootFault: return in->imageValid ? CvScreenWaitMenu : noImageScreen(in);
    case CvReasonBootFailed: return CvScreenWaitBootFailed;
    default: return noImageScreen(in);
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

// ---- Update session -----------------------------------------------------------------------

static void sessionClear(CvUpdateSession* s) {
  s->sessionAccepted = false;
  s->sessionRefused = false;
  s->sessionFailed = false;
  s->sessionStalled = false;
}

void cvSessionStart(CvUpdateSession* s, uint32_t reason, bool cableIn, uint32_t nowMs) {
  memset(s, 0, sizeof *s);
  s->reason = reason;
  s->timeoutFromMs = nowMs;
  cvDebounceReset(&s->vbus, cableIn);
}

// A self-powered device holds its D+ pull-up only while the cable carries VBUS. A pull leaves
// the USB stack mounted, so the board posts the unplug event VBUS sensing would. Without a write
// the timeout counts again from the pull. After a complete write the pull restarts the box.
CvCableAction cvSessionCable(CvUpdateSession* s, bool cableIn, uint32_t nowMs) {
  const bool first = !s->cableTracked;
  const bool changed = cvDebounceUpdate(&s->vbus, cableIn, nowMs, CV_VBUS_DEBOUNCE_MS);
  s->cableTracked = true;
  if (!first && !changed) return CvCableNone;
  if (s->vbus.level) {
    s->cableOut = false;
    return CvCableConnect;
  }
  if (first) return CvCableDisconnect;
  if (s->updateWritten) return CvCableRestart;
  s->cableOut = s->hostSeen;
  if (!s->writeBegun) {
    s->hostSeen = false;
    s->timeoutFromMs = nowMs;
  }
  return CvCableUnplugged;
}

bool cvSessionMount(CvUpdateSession* s, bool mounted) {
  if (mounted == s->mounted) return false;
  s->mounted = mounted;
  sessionClear(s);
  if (mounted) s->hostSeen = true;
  return mounted;
}

static uint32_t sectorBit(uint32_t address) {
  return 1u << ((address - CV_APP_START) / CV_FLASH_SECTOR_SIZE);
}

bool cvSessionSectorErased(const CvUpdateSession* s, uint32_t address, uint32_t sessionBlocks) {
  return sessionBlocks != 0u && address >= CV_APP_START && address < CV_APP_END &&
         (s->sessionSectors & sectorBit(address)) != 0u;
}

CvBlockVerdict cvSessionBlock(CvUpdateSession* s, const CvUf2BlockInfo* block, bool targetHoldsOtherData) {
  if (!cvUf2BlockAccepted(block->familyId, block->targetAddr, block->payloadSize, block->blockNo,
                          block->numBlocks)) {
    if (s->sessionAccepted) {
      s->sessionFailed = true;
    } else {
      s->sessionRefused = true;
    }
    s->diagDirty = true;
    return CvBlockRefused;
  }
  const bool erased = cvSessionSectorErased(s, block->targetAddr, block->sessionBlocks);
  if (block->sessionBlocks == 0u) s->sessionSectors = 0u;
  const CvUf2SessionInputs in = {block->sessionBlocks, block->numBlocks,  block->blockNo,
                                 erased,               erased && targetHoldsOtherData, s->sessionFailed};
  const bool restart = cvUf2SessionRestart(&in);
  if (restart) {
    s->updateWritten = false;
    s->ejectPending = false;
    s->sessionSectors = 0u;
    s->sessionRefused = false;
    s->sessionFailed = false;
  }
  s->sessionSectors |= sectorBit(block->targetAddr);
  s->sessionStalled = false;
  s->writeBegun = true;
  s->sessionAccepted = true;
  return restart ? CvBlockRestarted : CvBlockJoined;
}

void cvSessionBlockWritten(CvUpdateSession* s, bool written, uint32_t nowMs) {
  s->lastBlockMs = nowMs;
  if (written) return;
  s->sessionFailed = true;
  s->diagDirty = true;
}

void cvSessionWrongFile(CvUpdateSession* s, bool newName) {
  if (newName) s->diagDirty = true;
  if (!s->sessionAccepted) s->sessionRefused = true;
}

bool cvSessionLastBlock(CvUpdateSession* s, bool imageGood) {
  s->diagDirty = true;
  if (!imageGood) {
    s->sessionFailed = true;
    return false;
  }
  s->updateWritten = true;
  return true;
}

void cvSessionEjected(CvUpdateSession* s, uint32_t nowMs) {
  if (!s->writeBegun || s->ejectPending) return;
  s->ejectPending = true;
  s->ejectedMs = nowMs;
}

bool cvSessionTimeoutArmed(const CvUpdateSession* s, const struct CvBootBlock* block, CvImageStatus image) {
  return cvBootTimeoutAllowed(block, image, s->hostSeen, s->writeBegun);
}

CvTickAction cvSessionTick(CvUpdateSession* s, bool timeoutArmed, uint32_t nowMs) {
  if (s->ejectPending && nowMs - s->ejectedMs >= CV_EJECT_SETTLE_MS) return CvTickRestart;
  if (cvUpdateStalled(s->sessionAccepted, s->updateWritten, nowMs, s->lastBlockMs)) s->sessionStalled = true;
  if (timeoutArmed && nowMs - s->timeoutFromMs >= CV_UPDATE_TIMEOUT_MS) return CvTickTimeout;
  return CvTickNone;
}

CvScreen cvSessionScreen(const CvUpdateSession* s, bool imageValid, bool timeoutArmed) {
  if (s->updateWritten) return CvScreenDone;
  const CvScreenInputs in = {s->reason,          imageValid,        s->writeBegun,
                             s->sessionAccepted, s->sessionRefused, s->sessionFailed || s->sessionStalled,
                             s->cableOut,        timeoutArmed,      s->bootUpdated};
  return cvUpdateScreen(&in);
}

bool cvRegionReadAllowed(const CvBootDecision* decision, bool eraseFailed) {
  return decision->imageReadable && !eraseFailed;
}
