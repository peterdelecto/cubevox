// Checks the bootloader on the host. Covers the boot block contract, the fault record and the name
// functions both programs print from, and the bootloader's pure policy: the decision, the fault
// record, the option byte step, the UF2 filters and the update-mode screens. The host build
// compiles boot_decide.c as C99, which includes boot_block.h as C, so the header's own asserts hold
// there too.

#include "support.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "board/boot_block.h"
#include "boot_decide.h"

// The boot block table of the field update spec restated as literals, so a change to boot_block.h
// breaks the build here.
static_assert(CV_BOOT_BLOCK_ADDR == 0x2001FF00u && CV_FAULT_RECORD_ADDR == 0x2001FFC0u &&
                  CV_DTCM_END == 0x20020000u && CV_BOOT_BLOCK_SIZE == 256u,
              "boot: DTCM addresses");
static_assert(sizeof(CvBootBlock) == 0xC0, "boot: 192 bytes, so the fault record at +0xC0 is outside");
static_assert(CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, request) == 0x2001FF00u, "boot: request");
static_assert(CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, bootFails) == 0x2001FF04u, "boot: bootFails");
static_assert(CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, bootFailsCheck) == 0x2001FF08u,
              "boot: bootFailsCheck");
static_assert(CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, blockVersion) == 0x2001FF0Cu,
              "boot: blockVersion");
static_assert(CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, lastReason) == 0x2001FF10u, "boot: lastReason");
static_assert(CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, faultAddress) == 0x2001FF14u,
              "boot: faultAddress");
static_assert(CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, appSummary) == 0x2001FF18u, "boot: appSummary");
static_assert(CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, bootFaultPc) == 0x2001FF40u &&
                  CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, bootFaultCfsr) == 0x2001FF44u &&
                  CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, bootFaultHfsr) == 0x2001FF48u &&
                  CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, bootFaultBfar) == 0x2001FF4Cu,
              "boot: the bootloader's fault registers");
static_assert(CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, optionBytes) == 0x2001FF50u, "boot: optionBytes");
static_assert(CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, resetFlags) == 0x2001FF54u &&
                  sizeof(CvBootBlock::resetFlags) == 4,
              "boot: resetFlags");
static_assert(CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, blockSeal) == 0x2001FF58u &&
                  sizeof(CvBootBlock::blockSeal) == 4,
              "boot: blockSeal");
static_assert(CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, bootReserved) == 0x2001FF5Cu &&
                  sizeof(CvBootBlock::bootReserved) == 36,
              "boot: the bootloader's reserved bytes end at +0x80");
static_assert(CV_BOOT_BLOCK_VERSION == 3u, "boot: block version 3 added blockSeal");
static_assert(CV_BOOT_BLOCK_SEAL == (uint32_t{'C'} | uint32_t{'V'} << 8 | uint32_t{'B'} << 16 | uint32_t{'K'} << 24),
              "boot: the seal reads CVBK");
static_assert(CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, appFieldsVersion) == 0x2001FF80u &&
                  CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, appReserved) == 0x2001FF84u &&
                  sizeof(CvBootBlock::appReserved) == 60,
              "boot: the application's area is the last 64 bytes");
static_assert(sizeof(CvBootBlock::appSummary) == 40, "boot: appSummary is 40 bytes");
static_assert(sizeof(CvImageHeader) == 32, "boot: the image header is 32 bytes");
static_assert(offsetof(CvImageHeader, magic) == 0 && offsetof(CvImageHeader, headerVersion) == 4 &&
                  offsetof(CvImageHeader, length) == 8 && offsetof(CvImageHeader, crc32) == 12 &&
                  offsetof(CvImageHeader, version) == 16 && sizeof(CvImageHeader::version) == 16,
              "boot: image header offsets");
static_assert(CV_REQUEST_UPDATE == 0xF01669EFu, "boot: the request word is TinyUF2's double-tap magic");
// TinyUF2's quick-boot and erase-app magics, which the resume word must never equal.
static_assert(CV_REQUEST_RESUME != CV_REQUEST_UPDATE && CV_REQUEST_RESUME != 0xF02669EFu &&
                  CV_REQUEST_RESUME != 0xF5E80AB4u && CV_REQUEST_RESUME != 0u,
              "boot: the resume word is the bootloader's own");
static_assert(CV_IMAGE_MAGIC == (uint32_t{'C'} | uint32_t{'V'} << 8 | uint32_t{'F'} << 16 | uint32_t{'W'} << 24),
              "boot: the image magic spells CVFW in file order");
static_assert(CV_UF2_FAMILY_CUBEVOX != 0x6db66082u, "boot: the family id differs from the generic STM32H7 id");
static_assert(CV_APP_START == 0x08020000u && CV_APP_END == 0x08100000u && CV_IMAGE_HEADER_OFFSET == 0x400u &&
                  CV_BANK2_BASE == 0x08100000u && CV_BANK2_END == 0x08200000u,
              "boot: flash map");
static_assert(CV_USB_VID_BOOT == 0x1209u && CV_USB_PID_BOOT == 0x0001u, "boot: USB ids");
static_assert(CV_BOOT_FAIL_LIMIT == 3u && CV_UPDATE_TIMEOUT_MS == 60000u && CV_HEALTHY_MS == 5000u &&
                  CV_WATCHDOG_BOOT_MS == 16000u,
              "boot: limits");
static_assert(CV_FAULT_MAGIC == 0x46415533u, "boot: the fault magic reads FAU3");
static_assert(sizeof(CvFaultRecord) == 44 && offsetof(CvFaultRecord, magic) == 0 &&
                  offsetof(CvFaultRecord, kind) == 4 && offsetof(CvFaultRecord, pc) == 8 &&
                  offsetof(CvFaultRecord, lr) == 12 && offsetof(CvFaultRecord, cfsr) == 16 &&
                  offsetof(CvFaultRecord, hfsr) == 20 && offsetof(CvFaultRecord, bfar) == 24 &&
                  offsetof(CvFaultRecord, mmfar) == 28 && offsetof(CvFaultRecord, sr2) == 32 &&
                  offsetof(CvFaultRecord, eccFa2) == 36 && offsetof(CvFaultRecord, detail) == 40,
              "boot: fault record offsets");
static_assert(CvFaultKindHard == 0 && CvFaultKindMemManage == 1 && CvFaultKindBus == 2 &&
                  CvFaultKindUsage == 3 && CvFaultKindClock == 4,
              "boot: the fault kinds are numbered as the application's trampolines pass them");
static_assert(CvClockStepSupply == 1 && CvClockStepVoltageScale == 2 &&
                  CvClockStepOscillator == 3 && CvClockStepClockTree == 4 &&
                  CvClockStepPeripheralClocks == 5,
              "boot: the clock steps are numbered from 1");
static_assert(CvReasonBootFailed == 5 && CvReasonBootFault == 6 && CvReasonClockFailed == 7,
              "boot: reasons only ever gain values at the end");

namespace {

constexpr uint32_t kPowerOn = CV_RSR_PORRSTF | CV_RSR_BORRSTF | (1u << 22);  // POR, BOR and PIN
constexpr uint32_t kSoftware = CV_RSR_SFTRSTF | (1u << 22);
constexpr uint32_t kAppSector3 = 0x08065432u;

// A block as the bootloader leaves it after a healthy start: version set, counter 0 with its check.
CvBootBlock bootBlock() {
  CvBootBlock b;
  std::memset(&b, 0, sizeof b);
  cvBootBlockSeal(&b);
  cvBootFailsSet(&b, 0);
  return b;
}

CvBootInputs inputs(CvImageStatus image, uint32_t clockStep = 0) {
  return CvBootInputs{image, clockStep};
}

bool decided(const CvBootDecision& d, CvBootAction action, CvBootReason reason, uint32_t erase = 0) {
  return d.action == action && d.reason == reason && d.eraseAddress == erase;
}

// One start after a software reset: adopt, then decide. Checks the two writes every decision makes.
CvBootDecision decide(CvBootBlock& b, const CvBootInputs& in, uint32_t resetFlags = kSoftware) {
  cvBootAdopt(&b, resetFlags);
  const CvBootDecision d = cvBootDecide(&b, &in);
  check(b.request == 0, "boot: a decision consumes the request word");
  check(b.blockVersion == CV_BOOT_BLOCK_VERSION, "boot: a start writes the block version");
  return d;
}

CvBootDecision decide(CvBootBlock& b, CvImageStatus image) {
  return decide(b, inputs(image));
}

CvFaultInfo faultAt(uint32_t eccAddress, bool imageReading, bool updateMode) {
  return CvFaultInfo{kSoftware, true, eccAddress, imageReading, updateMode,
                     0x08001234u, 0x00008200u, 0x40000000u, eccAddress};
}

void recordFault(CvBootBlock& b, uint32_t eccAddress, bool imageReading = false, bool updateMode = false) {
  const CvFaultInfo fault = faultAt(eccAddress, imageReading, updateMode);
  cvBootRecordFault(&b, &fault);
}

// One block per row of the decision table, set up so that only that row applies.
void checkBootRows() {
  CvBootBlock b = bootBlock();
  b.request = CV_REQUEST_UPDATE;
  check(decided(decide(b, CvImageValid), CvActionUpdateMode, CvReasonAppRequest),
        "boot: row 2, request from the application");
  check(b.lastReason == CvReasonAppRequest, "boot: row 2 stores the reason");

  b = bootBlock();
  check(decided(decide(b, CvImageInvalid), CvActionUpdateMode, CvReasonImageInvalid),
        "boot: row 3, invalid image");
  check(b.lastReason == CvReasonImageInvalid, "boot: row 3 stores the reason");

  b = bootBlock();
  recordFault(b, kAppSector3);
  const CvBootDecision unreadable = decide(b, CvImageValid);
  check(decided(unreadable, CvActionUpdateMode, CvReasonImageUnreadable, 0x08060000u) && unreadable.imageReadable,
        "boot: row 4, read fault yields its sector, and the image is read after the erase");
  check(b.lastReason == CvReasonImageUnreadable && b.faultAddress == kAppSector3,
        "boot: row 4 keeps the reason and the address for DIAG.TXT");

  b = bootBlock();
  cvBootFailsSet(&b, CV_BOOT_FAIL_LIMIT);
  check(decided(decide(b, CvImageValid), CvActionUpdateMode, CvReasonBootFailed),
        "boot: row 5, three failed starts");
  check(cvBootFailsGet(&b) == CV_BOOT_FAIL_LIMIT && b.lastReason == CvReasonBootFailed,
        "boot: row 5 stores the reason and leaves the counter");

  b = bootBlock();
  check(decided(decide(b, CvImageValid), CvActionRunApp, CvReasonNone), "boot: row 6, run the application");
  check(cvBootFailsGet(&b) == 1 && cvBootFailsValid(&b), "boot: row 6 counts the start and writes the check word");
}

// Several rows apply at once and the higher row wins.
void checkBootPriority() {
  CvBootBlock b = bootBlock();
  b.request = CV_REQUEST_UPDATE;
  cvBootFailsSet(&b, CV_BOOT_FAIL_LIMIT);
  b.request = CV_REQUEST_UPDATE;
  check(decided(decide(b, CvImageInvalid), CvActionUpdateMode, CvReasonAppRequest),
        "boot: a request beats an invalid image and a failed count");
  check(decided(decide(b, CvImageInvalid), CvActionUpdateMode, CvReasonImageInvalid),
        "boot: an invalid image beats a failed count");
  check(decided(decide(b, CvImageValid), CvActionUpdateMode, CvReasonBootFailed),
        "boot: a failed count alone stays in update mode");

  b = bootBlock();
  recordFault(b, kAppSector3);
  cvBootFailsSet(&b, CV_BOOT_FAIL_LIMIT);
  check(decided(decide(b, CvImageInvalid), CvActionUpdateMode, CvReasonImageUnreadable, 0x08060000u),
        "boot: a pending read fault beats an invalid image and a failed count");

  b = bootBlock();
  b.request = CV_REQUEST_UPDATE;
  recordFault(b, 0u);
  check(decided(decide(b, CvImageValid), CvActionUpdateMode, CvReasonBootFault),
        "boot: the bootloader's resume request names its own reason");
}

void checkBootCounter() {
  CvBootBlock b = bootBlock();
  b.bootFails = 7;
  b.bootFailsCheck = 0x12345678u;
  check(!cvBootFailsValid(&b) && cvBootFailsGet(&b) == 0, "boot: a counter whose check word disagrees reads as 0");
  check(decided(decide(b, CvImageValid), CvActionRunApp, CvReasonNone) && cvBootFailsGet(&b) == 1 &&
            cvBootFailsValid(&b),
        "boot: with a garbage check word the next start counts as the first");

  b = bootBlock();
  for (uint32_t n = 1; n <= CV_BOOT_FAIL_LIMIT; ++n) {
    const CvBootDecision d = decide(b, CvImageValid);
    check(d.action == CvActionRunApp && cvBootFailsGet(&b) == n, "boot: starts below the limit run and count");
  }
  for (int i = 0; i < 3; ++i) {
    check(decided(decide(b, CvImageValid), CvActionUpdateMode, CvReasonBootFailed) &&
              cvBootFailsGet(&b) == CV_BOOT_FAIL_LIMIT,
          "boot: at the limit update mode stays and the counter stays put");
  }

  b = bootBlock();
  cvBootFailsSet(&b, 0xFFFFFFFFu);
  check(decided(decide(b, CvImageValid), CvActionUpdateMode, CvReasonBootFailed) &&
            cvBootFailsGet(&b) == 0xFFFFFFFFu,
        "boot: a huge valid count stays in update mode without wrapping");
}

// The flags are RCC_RSR as the bootloader read it at this start, before it cleared the register.
void checkBootPowerOnFlags() {
  struct Case {
    uint32_t flags;
    bool powerOn;
    const char* what;
  };
  const Case cases[] = {
      {kPowerOn, true, "power-on with POR, BOR and PIN"},
      {CV_RSR_PORRSTF, true, "POR alone"},
      {CV_RSR_BORRSTF | (1u << 22), true, "a brown-out"},
      {kPowerOn | CV_RSR_SFTRSTF, false, "power-on flags kept through a software reset"},
      {kPowerOn | CV_RSR_IWDG1RSTF, false, "power-on flags kept through a watchdog reset"},
      {kPowerOn | CV_RSR_WWDG1RSTF, false, "power-on flags kept through a window watchdog reset"},
      {kSoftware, false, "a software reset"},
      {1u << 22, false, "the reset pin alone"},
      {0u, false, "no flag"},
  };
  char message[112];
  for (const Case& c : cases) {
    std::snprintf(message, sizeof message, "boot: %s %s a power-on", c.what, c.powerOn ? "is" : "is not");
    check(cvBootPowerOn(c.flags) == c.powerOn, message);
  }
}

// A power cycle is not a failed start, and every other reset still counts.
void checkBootPowerOn() {
  CvBootBlock b = bootBlock();
  cvBootFailsSet(&b, 2);
  b.request = CV_REQUEST_UPDATE;
  b.lastReason = CvReasonBootFailed;
  std::strcpy(b.appSummary, "v2.00.10 up 5 s");
  b.appFieldsVersion = 3;
  b.optionBytes = CvOptionTimeout;
  check(decided(decide(b, inputs(CvImageValid), kPowerOn), CvActionRunApp, CvReasonNone) &&
            cvBootFailsGet(&b) == 1,
        "boot: a power-on clears the counter and the request, and the start counts as the first");
  check(b.lastReason == CvReasonNone && b.appSummary[0] == '\0' && b.appFieldsVersion == 0 &&
            b.optionBytes == CvOptionUnchecked,
        "boot: a power-on clears every field, the application's included");

  b = bootBlock();
  for (int dip = 0; dip < 5; ++dip) {
    check(decide(b, inputs(CvImageValid), kPowerOn).action == CvActionRunApp,
          "boot: power dips inside the healthy window never reach Startup failed");
  }
}

void checkBootRestartsCount() {
  CvBootBlock b = bootBlock();
  decide(b, inputs(CvImageValid), kPowerOn);
  for (uint32_t n = 2; n <= CV_BOOT_FAIL_LIMIT; ++n) {
    check(decide(b, inputs(CvImageValid), kPowerOn | CV_RSR_SFTRSTF).action == CvActionRunApp &&
              cvBootFailsGet(&b) == n,
          "boot: a crash before the application clears the flags still counts");
  }
  check(decided(decide(b, inputs(CvImageValid), kPowerOn | CV_RSR_IWDG1RSTF), CvActionUpdateMode,
                CvReasonBootFailed),
        "boot: a hang the watchdog ends counts, so the third failure stays in update mode");

  b = bootBlock();
  cvBootAdopt(&b, kPowerOn);
  recordFault(b, kAppSector3, true, false);
  check(decided(decide(b, inputs(CvImageInvalid), kPowerOn | CV_RSR_SFTRSTF), CvActionUpdateMode,
                CvReasonImageUnreadable, 0x08060000u),
        "boot: a read fault in the first start after power-on survives its own reset");
}

// DTCM is random after power-on. Whatever the bytes, the first start is a first start and every
// field comes out clean.
void checkBootGarbage() {
  const uint8_t fills[] = {0x00, 0xA5, 0xFF};
  for (const uint8_t fill : fills) {
    CvBootBlock b;
    std::memset(&b, fill, sizeof b);
    check(decided(decide(b, inputs(CvImageValid), kPowerOn), CvActionRunApp, CvReasonNone) &&
              cvBootFailsGet(&b) == 1,
          "boot: a garbage block at power-on gives a first start");
    bool clean = b.lastReason == CvReasonNone && b.faultAddress == 0 && b.bootFaultPc == 0 &&
                 b.optionBytes == CvOptionUnchecked && b.appFieldsVersion == 0;
    for (const char ch : b.appSummary) clean = clean && ch == '\0';
    for (const uint8_t byte : b.bootReserved) clean = clean && byte == 0;
    for (const uint8_t byte : b.appReserved) clean = clean && byte == 0;
    check(clean, "boot: a garbage block at power-on comes out with every field clear");
  }

  CvBootBlock b;
  std::memset(&b, 0xA5, sizeof b);
  b.blockVersion = 0;
  check(decided(decide(b, CvImageValid), CvActionRunApp, CvReasonNone) && cvBootFailsGet(&b) == 1 &&
            b.appSummary[0] == '\0',
        "boot: a block no bootloader initialised is cleared on any reset");
}

// The ST ROM loader leaves other data in DTCM and ends its session with a system reset, so the
// bootloader sees a block of random bytes after a reset that keeps DTCM. It clears the block and
// reports no fault.
void checkBootJunkAfterRomSession() {
  uint32_t state = 0x9E3779B9u;
  for (int run = 0; run < 256; ++run) {
    CvBootBlock b;
    auto* bytes = reinterpret_cast<uint8_t*>(&b);
    for (size_t i = 0; i < sizeof b; ++i) {
      state = state * 1664525u + 1013904223u;
      bytes[i] = static_cast<uint8_t>(state >> 24);
    }
    const CvBootDecision d = decide(b, inputs(CvImageValid), kSoftware);
    check(decided(d, CvActionRunApp, CvReasonNone) && cvBootFailsGet(&b) == 1,
          "boot: a junk block after a system reset gives a first start");
    check(b.bootFaultPc == 0 && b.bootFaultCfsr == 0 && b.bootFaultHfsr == 0 && b.bootFaultBfar == 0,
          "boot: a junk block after a system reset reports no bootloader fault");
    bool clean = b.lastReason == CvReasonNone && b.faultAddress == 0 && b.optionBytes == CvOptionUnchecked &&
                 b.appFieldsVersion == 0 && b.resetFlags == kSoftware;
    for (const char ch : b.appSummary) clean = clean && ch == '\0';
    for (const uint8_t byte : b.appReserved) clean = clean && byte == 0;
    check(clean, "boot: a junk block after a system reset comes out with every field clear");
  }
}

// The seal separates a block a cubevox bootloader wrote from other data. The four pairings of old
// and new bootloader with old and new application each read the block the way they did before, and a
// real bootloader fault survives the reset that follows it.
void checkBootSeal() {
  // The bootloader's own fault, recorded before a system reset, reaches the application.
  CvBootBlock b = bootBlock();
  recordFault(b, 0u);
  decide(b, inputs(CvImageValid), kSoftware);
  check(b.bootFaultPc == 0x08001234u && b.bootFaultCfsr == 0x00008200u && cvBootBlockInitialised(&b),
        "boot: a real bootloader fault survives the reset and the application accepts the block");

  // Counter and update request survive a system reset as before.
  b = bootBlock();
  cvBootFailsSet(&b, 2);
  b.request = CV_REQUEST_UPDATE;
  std::strcpy(b.appSummary, "v2.01.52 up 7 s");
  check(decided(decide(b, CvImageValid), CvActionUpdateMode, CvReasonAppRequest) &&
            cvBootFailsGet(&b) == 2 && std::strcmp(b.appSummary, "v2.01.52 up 7 s") == 0,
        "boot: a sealed block keeps its counter, request and summary across a system reset");

  // New application, old bootloader: a version 2 block has no seal and is read as before.
  b = bootBlock();
  b.blockVersion = 2;
  b.blockSeal = 0;
  b.resetFlags = kSoftware;
  check(cvBootBlockInitialised(&b) && cvBootBlockResetFlags(&b) == kSoftware,
        "boot: the application still trusts a block from a version 2 bootloader");

  // New application, no bootloader write: version 3 and up without the seal is not a block.
  b = bootBlock();
  b.blockSeal = 0;
  b.resetFlags = kSoftware;
  check(!cvBootBlockInitialised(&b) && cvBootBlockResetFlags(&b) == 0,
        "boot: the application rejects a version 3 block without the seal");

  // New bootloader, block left by an old bootloader: no seal, so it is cleared once.
  b = bootBlock();
  b.blockVersion = 2;
  b.blockSeal = 0;
  cvBootFailsSet(&b, 1);
  check(decided(decide(b, CvImageValid), CvActionRunApp, CvReasonNone) && cvBootFailsGet(&b) == 1 &&
            cvBootBlockSealed(&b),
        "boot: a version 2 block is cleared by the new bootloader and comes out sealed");

  // Old application, new bootloader: it checks blockVersion >= 1 only and never writes the seal.
  b = bootBlock();
  check(b.blockVersion >= 1u, "boot: the version 3 block passes an old application's version check");
}

// The bootloader hands its reset flags to the application in the block, because the jump clears
// RCC_RSR. Every start overwrites them, and an application reading a block from a bootloader older
// than version 2 gets none.
void checkBootResetFlags() {
  CvBootBlock b = bootBlock();
  decide(b, inputs(CvImageValid), kPowerOn);
  check(b.resetFlags == kPowerOn, "boot: a start stores the flags it read in the block");
  decide(b, inputs(CvImageValid), kSoftware);
  check(b.resetFlags == kSoftware, "boot: the next start replaces them with its own");
  check(cvBootBlockResetFlags(&b) == kSoftware,
        "boot: the application takes the flags from the block when the live register reads 0");

  CvBootBlock garbage;
  std::memset(&garbage, 0xA5, sizeof garbage);
  decide(garbage, inputs(CvImageValid), kPowerOn);
  check(garbage.resetFlags == kPowerOn, "boot: a power-on clears the block and then stores the flags");

  CvFaultInfo early = faultAt(0u, false, false);
  early.blockAdopted = false;
  early.resetFlags = kPowerOn;
  std::memset(&garbage, 0xA5, sizeof garbage);
  cvBootRecordFault(&garbage, &early);
  check(garbage.resetFlags == kPowerOn, "boot: a fault before the block was adopted stores the flags too");

  b = bootBlock();
  b.blockVersion = 1;
  b.resetFlags = 0xDEADBEEFu;
  check(cvBootBlockResetFlags(&b) == 0, "boot: a version 1 block carries no flags, whatever its bytes hold");
  b.blockVersion = 0;
  check(cvBootBlockResetFlags(&b) == 0, "boot: an uninitialised block carries no flags");
}

// Fields the last run left out of range after a reset that keeps DTCM.
void checkBootStaleFields() {
  CvBootBlock b = bootBlock();
  b.lastReason = 0xDEADBEEFu;
  decide(b, CvImageValid);
  check(b.lastReason == CvReasonNone, "boot: an out-of-range reason is cleared, it indexes name tables");

  b = bootBlock();
  b.lastReason = 0xDEADBEEFu;
  b.request = CV_REQUEST_UPDATE;
  check(decided(decide(b, CvImageValid), CvActionUpdateMode, CvReasonAppRequest),
        "boot: a request with a garbage reason is a plain request");

  b = bootBlock();
  std::strcpy(b.appSummary, "v2.00.10 up 5 s");
  b.request = CV_REQUEST_UPDATE;
  decide(b, CvImageValid);
  check(std::strcmp(b.appSummary, "v2.00.10 up 5 s") == 0, "boot: a decision keeps the application's summary");
}

// blockVersion is the bootloader's, every version from 1 up counts as initialised, and the
// application's own area survives decisions, faults and updates.
void checkBootVersions() {
  CvBootBlock b = bootBlock();
  b.blockVersion = CV_BOOT_BLOCK_VERSION + 1u;
  std::strcpy(b.appSummary, "v3.00.00 up 9 s");
  b.appFieldsVersion = 2;
  b.appReserved[0] = 0x5A;
  check(cvBootBlockInitialised(&b), "boot: a later block version counts as initialised");
  decide(b, CvImageValid);
  check(std::strcmp(b.appSummary, "v3.00.00 up 9 s") == 0 && b.appFieldsVersion == 2 && b.appReserved[0] == 0x5A,
        "boot: a later block version keeps the application's summary and fields");

  recordFault(b, 0u);
  decide(b, CvImageValid);
  cvBootMarkUpdated(&b);
  check(b.appFieldsVersion == 2 && b.appReserved[0] == 0x5A,
        "boot: faults and updates never touch the application's area");

  b = bootBlock();
  b.blockVersion = 0;
  check(!cvBootBlockInitialised(&b), "boot: block version 0 is uninitialised");
}

// The sector to erase for the address of a read fault.
void checkBootReadFault() {
  struct Case {
    uint32_t address;
    uint32_t erase;
    CvBootReason reason;
    const char* what;
  };
  const Case cases[] = {
      {kAppSector3, 0x08060000u, CvReasonImageUnreadable, "an address in sector 3"},
      {0x08020000u, 0x08020000u, CvReasonImageUnreadable, "the first application word"},
      {0x080FFFFCu, 0x080E0000u, CvReasonImageUnreadable, "the last application word"},
      {0x0801FFFCu, 0u, CvReasonImageInvalid, "the bootloader sector, which is never erased"},
      {0x08200000u, 0u, CvReasonImageInvalid, "the first address past flash"},
      {0xFFFFFFFFu, 0u, CvReasonImageInvalid, "a garbage address"},
  };
  char message[96];
  for (const Case& c : cases) {
    CvBootBlock b = bootBlock();
    recordFault(b, c.address);
    check(b.request == CV_REQUEST_RESUME && b.lastReason == CvReasonImageUnreadable && b.faultAddress == c.address,
          "boot: recording a read fault sets the resume request, the reason and the address");
    std::snprintf(message, sizeof message, "boot: read fault at %s", c.what);
    const CvBootDecision d = decide(b, CvImageInvalid);
    check(decided(d, CvActionUpdateMode, c.reason, c.erase) && d.imageReadable == (c.erase != 0), message);
    check(b.faultAddress == c.address, "boot: the fault address stays for DIAG.TXT");
  }
}

// What a read fault leaves for the starts after the one that erases its sector.
void checkBootReadFaultAfter() {
  CvBootBlock b = bootBlock();
  recordFault(b, 0u, true, false);
  check(b.request == CV_REQUEST_RESUME && b.lastReason == CvReasonImageUnreadable && b.faultAddress == 0,
        "boot: any fault while the image is read is an unreadable image");
  const CvBootDecision blind = decide(b, CvImageInvalid);
  check(decided(blind, CvActionUpdateMode, CvReasonImageInvalid) && !blind.imageReadable,
        "boot: an unreadable image with no sector to erase is not read again, so the fault cannot loop");

  b = bootBlock();
  recordFault(b, kAppSector3);
  decide(b, CvImageInvalid);
  check(decided(decide(b, CvImageInvalid), CvActionUpdateMode, CvReasonImageInvalid) && b.faultAddress == 0,
        "boot: the sector is yielded once, a later start sees a plain invalid image");

  b = bootBlock();
  b.lastReason = CvReasonImageUnreadable;
  b.request = CV_REQUEST_UPDATE;
  b.faultAddress = kAppSector3;
  check(decided(decide(b, CvImageValid), CvActionUpdateMode, CvReasonAppRequest),
        "boot: the application's request never reads as a read fault, whatever reason is left over");
}

// A fault in bank 2 is a read of the diagnostic log. It keeps the address, erases nothing, and in
// update mode brings the box back to update mode with the same reason.
void checkBootBank2Fault() {
  const uint32_t addresses[] = {0x08100000u, 0x08145678u, 0x081FFFE0u};
  const CvBootReason reasons[] = {CvReasonAppRequest, CvReasonImageInvalid,
                                  CvReasonBootFailed};
  for (const uint32_t address : addresses) {
    for (const CvBootReason reason : reasons) {
      CvBootBlock b = bootBlock();
      b.lastReason = reason;
      recordFault(b, address, false, true);
      check(b.request == CV_REQUEST_RESUME && b.lastReason == reason && b.faultAddress == address,
            "boot: a bank 2 fault in update mode asks to resume and keeps the reason and the address");
      const CvBootDecision d = decide(b, CvImageInvalid);
      check(decided(d, CvActionUpdateMode, reason) && d.imageReadable && b.faultAddress == address,
            "boot: after a bank 2 fault update mode resumes for the same reason, with no erase");
    }

    CvBootBlock b = bootBlock();
    recordFault(b, address, false, false);
    check(b.request == 0 && b.faultAddress == address, "boot: a bank 2 fault outside update mode asks for nothing");
    check(decided(decide(b, CvImageValid), CvActionRunApp, CvReasonNone) && b.faultAddress == address,
          "boot: after a bank 2 fault a valid image runs and the address stays");
    check(decided(decide(b, CvImageValid), CvActionRunApp, CvReasonNone) && b.faultAddress == address &&
              cvBootFailsGet(&b) == 2,
          "boot: the address outlasts further starts until the application clears it");
  }

  CvBootBlock b = bootBlock();
  recordFault(b, 0x08145678u, false, true);
  recordFault(b, kAppSector3, false, true);
  check(decided(decide(b, CvImageInvalid), CvActionUpdateMode, CvReasonImageUnreadable, 0x08060000u),
        "boot: an application fault after a bank 2 fault still yields its sector");
}

// Any other fault of the bootloader's own: registers kept, update mode for BootFault, and the
// timeout runs when the image is valid.
void checkBootFaultRegisters() {
  CvBootBlock b = bootBlock();
  recordFault(b, 0u, false, true);
  check(b.request == CV_REQUEST_RESUME && b.lastReason == CvReasonBootFault && b.bootFaultPc == 0x08001234u &&
            b.bootFaultCfsr == 0x8200u && b.bootFaultHfsr == 0x40000000u && b.faultAddress == 0,
        "boot: a bootloader fault records its registers and asks for update mode");
  const CvBootDecision d = decide(b, CvImageInvalid);
  check(decided(d, CvActionUpdateMode, CvReasonBootFault) && d.imageReadable,
        "boot: a bootloader fault returns to update mode and the image is read after the decision");
  check(cvBootTimeoutAllowed(&b, CvImageValid, false, false), "boot: a bootloader fault times out on a valid image");
  check(!cvBootTimeoutAllowed(&b, CvImageInvalid, false, false), "boot: never on an invalid one");
  check(decided(decide(b, CvImageValid), CvActionRunApp, CvReasonNone) && b.bootFaultPc == 0x08001234u,
        "boot: after the timeout the application runs and the registers stay for its report");

  cvBootMarkUpdated(&b);
  check(b.bootFaultPc == 0 && b.bootFaultCfsr == 0 && b.bootFaultHfsr == 0 && b.bootFaultBfar == 0,
        "boot: an update clears the bootloader's fault registers");

  CvBootBlock garbage;
  std::memset(&garbage, 0xA5, sizeof garbage);
  CvFaultInfo early = faultAt(kAppSector3, true, false);
  early.blockAdopted = false;
  early.resetFlags = kPowerOn;
  cvBootRecordFault(&garbage, &early);
  check(decided(decide(garbage, inputs(CvImageInvalid), kPowerOn | CV_RSR_SFTRSTF), CvActionUpdateMode,
                CvReasonImageUnreadable, 0x08060000u) &&
            garbage.appSummary[0] == '\0',
        "boot: a fault before the block was adopted adopts it first, so its record survives clean");
}

// A bootloader fault that repeats never loops the box out of its image or its update mode.
void checkBootFaultRepeats() {
  CvBootBlock b = bootBlock();
  recordFault(b, 0u, false, true);
  decide(b, CvImageInvalid);
  recordFault(b, 0u, false, true);
  check(b.request == 0 && b.lastReason == CvReasonBootFault,
        "boot: a second fault inside the update mode a first one started asks for nothing");
  check(decided(decide(b, CvImageValid), CvActionRunApp, CvReasonNone),
        "boot: so a bootloader fault that repeats in update mode cannot keep a valid image from running");

  b = bootBlock();
  int faultScreens = 0;
  for (int start = 0; start < 12 && b.lastReason != CvReasonBootFailed; ++start) {
    const CvBootReason reason = decide(b, CvImageValid).reason;
    if (reason == CvReasonNone) {
      recordFault(b, 0u, false, false);  // a fault on the way to the application, every time
    } else if (reason == CvReasonBootFault) {
      ++faultScreens;  // the timeout runs out and the next start decides again
    }
  }
  check(b.lastReason == CvReasonBootFailed && faultScreens == CV_BOOT_FAIL_LIMIT,
        "boot: a fault before every jump ends in Startup failed through the start counter");
}

// The inputs are the image and the clock step, so no pin can change a decision. MenuHeld stays in
// the enum for old logs and no start produces it.
void checkBootNoMenuReason() {
  static_assert(sizeof(CvBootInputs) == sizeof(CvImageStatus) + sizeof(uint32_t),
                "CvBootInputs carries the image and the clock step only");
  int menuReasons = 0;
  for (const uint32_t request : {0u, CV_REQUEST_UPDATE, CV_REQUEST_RESUME}) {
    for (const uint32_t last : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u}) {
      for (const uint32_t fails : {0u, CV_BOOT_FAIL_LIMIT}) {
        for (const CvImageStatus image : {CvImageValid, CvImageInvalid}) {
          for (const uint32_t clock : {0u, static_cast<uint32_t>(CvClockStepOscillator)}) {
            CvBootBlock b = bootBlock();
            b.request = request;
            b.lastReason = last;
            cvBootFailsSet(&b, fails);
            if (decide(b, inputs(image, clock)).reason == CvReasonMenuHeld) ++menuReasons;
          }
        }
      }
    }
  }
  check(menuReasons == 0, "boot: no block, image or clock produces the MenuHeld reason");

  CvBootBlock b = bootBlock();
  b.request = CV_REQUEST_RESUME;
  b.lastReason = CvReasonMenuHeld;
  check(decided(decide(b, CvImageValid), CvActionUpdateMode, CvReasonBootFault),
        "boot: a resume with the reserved MenuHeld reason reads as a bootloader fault");
  check(decided(decide(b = bootBlock(), CvImageValid), CvActionRunApp, CvReasonNone),
        "boot: a healthy start runs the application with nothing else to read");
}

void checkBootClock() {
  CvBootBlock b = bootBlock();
  check(decided(decide(b, inputs(CvImageValid, CvClockStepOscillator)), CvActionUpdateMode,
                CvReasonClockFailed),
        "boot: a failed clock beats everything else, because the application cannot run");
  check(!cvBootTimeoutAllowed(&b, CvImageValid, false, false), "boot: a failed clock never times out");

  b = bootBlock();
  recordFault(b, kAppSector3);
  check(decided(decide(b, inputs(CvImageValid, CvClockStepClockTree)), CvActionUpdateMode,
                CvReasonClockFailed, 0x08060000u),
        "boot: a failed clock still erases the sector a read fault names");

  b = bootBlock();
  check(decide(b, inputs(CvImageValid, 0)).action == CvActionRunApp, "boot: clock step 0 is a running clock");
}

void checkBootTimeout() {
  int wrong = 0;
  for (uint32_t reason = CvReasonNone; reason <= CvReasonClockFailed; ++reason) {
    for (int mask = 0; mask < 8; ++mask) {
      const CvImageStatus image = (mask & 1) ? CvImageInvalid : CvImageValid;
      const bool host = (mask & 2) != 0;
      const bool written = (mask & 4) != 0;
      CvBootBlock b = bootBlock();
      b.lastReason = reason;
      const bool leavable =
          reason == CvReasonAppRequest || reason == CvReasonBootFault;
      const bool want = leavable && image == CvImageValid && !host && !written;
      if (cvBootTimeoutAllowed(&b, image, host, written) != want) ++wrong;
    }
  }
  check(wrong == 0, "boot: the timeout needs AppRequest or BootFault, a valid image, no host, no write");

  CvBootBlock b = bootBlock();
  b.request = CV_REQUEST_UPDATE;
  decide(b, CvImageValid);
  check(cvBootTimeoutAllowed(&b, CvImageValid, false, false), "boot: a menu entry with no computer times out");
  check(!cvBootTimeoutAllowed(&b, CvImageValid, true, false), "boot: a host that enumerated ends the timeout");
  check(!cvBootTimeoutAllowed(&b, CvImageValid, false, true), "boot: a write that began ends the timeout");
  check(!cvBootTimeoutAllowed(&b, CvImageInvalid, false, false), "boot: an invalid image never times out");
}

void checkBootMarkUpdated() {
  CvBootBlock b = bootBlock();
  cvBootFailsSet(&b, CV_BOOT_FAIL_LIMIT);
  b.request = CV_REQUEST_UPDATE;
  b.lastReason = CvReasonBootFailed;
  b.faultAddress = kAppSector3;
  std::strcpy(b.appSummary, "v2.00.09 up 812 s");
  cvBootMarkUpdated(&b);
  check(cvBootFailsGet(&b) == 0 && cvBootFailsValid(&b), "boot: an update zeroes the counter with its check word");
  check(b.request == 0 && b.lastReason == CvReasonNone && b.faultAddress == 0,
        "boot: an update clears the request, the reason and the fault address");
  check(b.appSummary[0] == '\0' && b.blockVersion == CV_BOOT_BLOCK_VERSION,
        "boot: an update clears the old application's summary");
  check(decided(decide(b, CvImageValid), CvActionRunApp, CvReasonNone),
        "boot: after an update the next start runs the application");

  // A torn log word DIAG.TXT faulted on, then an update in the same update mode.
  constexpr uint32_t kLogWord = CV_DIAG_LOG_ADDR + 0x1230u;
  b = bootBlock();
  b.request = CV_REQUEST_UPDATE;
  decide(b, CvImageValid);
  recordFault(b, kLogWord, false, true);
  decide(b, CvImageValid);
  cvBootMarkUpdated(&b);
  check(b.faultAddress == kLogWord, "boot: an update keeps a bank 2 fault address for the new application");
  decide(b, CvImageValid);
  check(b.faultAddress == kLogWord, "boot: and the start into the new image still carries it");
}

// The updater's mark after a bootloader write: it survives the restart into the new bootloader,
// changes no decision, and goes with the next firmware install or a power-on.
void checkBootUpdatedMark() {
  CvBootBlock plain = bootBlock();
  CvBootBlock marked = bootBlock();
  check(!cvBootUpdatedMarked(&plain), "mark: a clean block carries no bootloader-updated mark");
  cvBootUpdatedMarkSet(&marked);
  check(cvBootUpdatedMarked(&marked), "mark: the updater's mark reads back");
  const CvBootDecision withMark = decide(marked, CvImageInvalid);
  const CvBootDecision without = decide(plain, CvImageInvalid);
  check(decided(withMark, without.action, without.reason, without.eraseAddress) &&
            withMark.imageReadable == without.imageReadable,
        "mark: the decision with the mark is the decision without it");
  check(decided(withMark, CvActionUpdateMode, CvReasonImageInvalid) && cvBootUpdatedMarked(&marked),
        "mark: it survives the restart into the new bootloader, which waits with no image");

  cvBootMarkUpdated(&marked);
  check(!cvBootUpdatedMarked(&marked), "mark: a complete firmware install clears it");

  CvBootBlock cut = bootBlock();
  cvBootUpdatedMarkSet(&cut);
  decide(cut, inputs(CvImageInvalid), kPowerOn);
  check(!cvBootUpdatedMarked(&cut), "mark: a power-on clears it");

  CvBootBlock half = bootBlock();
  cvBootUpdatedWordsPut(&half, CV_BOOT_UPDATED_MARK, 0u);
  check(!cvBootUpdatedMarked(&half), "mark: the magic without its complement is no mark");
  cvBootUpdatedWordsPut(&half, 0xFFFFFFFFu, 0xFFFFFFFFu);
  check(!cvBootUpdatedMarked(&half), "mark: erased bytes are no mark");

  // Update mode at the new bootloader's start: the mark picks the screen and nothing else.
  CvUpdateSession s;
  cvSessionStart(&s, CvReasonImageInvalid, true, 0u);
  s.bootUpdated = true;
  check(cvSessionScreen(&s, false, false) == CvScreenBootUpdated, "mark: update mode says the bootloader updated");
  check(!cvSessionTimeoutArmed(&s, &plain, CvImageInvalid), "mark: no image, so no timeout, as without it");
}

// BOR level 3 and sector 0 write protection, programmed once per power-on at most.
void checkOptionBytes() {
  constexpr uint32_t kBor3 = 3u << CV_OPTSR_BOR_LEV_POS;
  constexpr uint32_t kFactory = 0x1016AAF0u & ~CV_OPTSR_BOR_LEV_MASK;  // RDP level 0 (0xAA), BOR off
  check(cvOptionsWanted(kFactory, 0xFFu), "options: a factory part wants both");
  check(cvOptionsWanted(kFactory | kBor3, 0xFFu), "options: an unprotected sector 0 is wanted");
  check(cvOptionsWanted(kFactory | (2u << CV_OPTSR_BOR_LEV_POS), 0xFEu), "options: BOR level 2 is wanted");
  check(!cvOptionsWanted(kFactory | kBor3, 0xFEu), "options: BOR 3 with sector 0 protected is done");

  check(cvOptionsOptsr(kFactory) == (kFactory | kBor3), "options: only BOR_LEV changes, RDP stays at level 0");
  check(cvOptionsOptsr(0xFFFFFFFFu) == 0xFFFFFFFFu && cvOptionsOptsr(0u) == kBor3,
        "options: BOR_LEV is set without touching any other bit");
  check(cvOptionsWpsn(0xFFu) == 0xFEu && cvOptionsWpsn(0x7Eu) == 0x7Eu && cvOptionsWpsn(0xFFFFFFFFu) == 0xFFFFFFFEu,
        "options: only sector 0's bit clears");

  CvBootBlock b = bootBlock();
  check(cvOptionsAttempt(&b, kFactory, 0xFFu), "options: the first start after power-on programs");
  const uint32_t failures[] = {CvOptionLocked, CvOptionTimeout, CvOptionChangeError, CvOptionNotApplied,
                               CvOptionInterrupted};
  for (const uint32_t failure : failures) {
    b.optionBytes = failure;
    check(cvOptionResultFailed(failure), "options: every failure result names a failure");
    check(!cvOptionsAttempt(&b, kFactory, 0xFFu), "options: a failure is not retried before the next power-on");
  }
  check(!cvOptionResultFailed(CvOptionUnchecked) && !cvOptionResultFailed(CvOptionAlreadySet) &&
            !cvOptionResultFailed(CvOptionProgrammed) && !cvOptionResultFailed(CV_DIAG_UNKNOWN_U32),
        "options: unchecked, set, programmed and an entry's blank field are not failures");
  cvBootAdopt(&b, kPowerOn);
  check(cvOptionsAttempt(&b, kFactory, 0xFFu), "options: a power-on allows one more attempt");
  b.optionBytes = CvOptionProgrammed;
  check(!cvOptionsAttempt(&b, kFactory | kBor3, 0xFEu), "options: nothing to do once both are set");
}

void checkUf2Filter() {
  struct Case {
    uint32_t family;
    uint32_t address;
    uint32_t length;
    uint32_t blockNo;
    uint32_t numBlocks;
    bool accepted;
    const char* what;
  };
  constexpr uint32_t kFull = (CV_APP_END - CV_APP_START) / 256u;  // 3584 blocks fill the region
  constexpr uint32_t kBlock5 = CV_APP_START + 5u * 256u;
  const Case cases[] = {
      {CV_UF2_FAMILY_CUBEVOX, CV_APP_START, 256u, 0u, 700u, true, "the first block"},
      {CV_UF2_FAMILY_CUBEVOX, kBlock5, 256u, 5u, 700u, true, "a block in its place"},
      {CV_UF2_FAMILY_CUBEVOX, CV_APP_END - 256u, 256u, kFull - 1u, kFull, true,
       "the last block of a file that fills the region"},
      {0x6DB66082u, CV_APP_START, 256u, 0u, 700u, false, "the generic STM32H7 family"},
      {CV_UF2_FAMILY_CUBEVOX, CV_APP_START - 256u, 256u, 0u, 700u, false, "the bootloader sector"},
      {CV_UF2_FAMILY_CUBEVOX, CV_APP_END, 256u, kFull, kFull + 1u, false, "bank 2"},
      {CV_UF2_FAMILY_CUBEVOX, CV_APP_END - 32u, 256u, 0u, 1u, false, "a block that runs past the region"},
      {CV_UF2_FAMILY_CUBEVOX, CV_APP_START + 16u, 256u, 0u, 1u, false, "half a flash word"},
      {CV_UF2_FAMILY_CUBEVOX, CV_APP_START, 100u, 0u, 1u, false, "a payload of part words"},
      {CV_UF2_FAMILY_CUBEVOX, CV_APP_START, 0u, 0u, 1u, false, "an empty payload"},
      {CV_UF2_FAMILY_CUBEVOX, 0xFFFFFF00u, 0x200u, 0u, 1u, false, "an address that wraps"},
      {CV_UF2_FAMILY_CUBEVOX, kBlock5, 256u, 0x80u + 5u, 700u, false,
       "an in-region block of a file based at 0x08018000"},
      {CV_UF2_FAMILY_CUBEVOX, kBlock5, 256u, 4u, 700u, false, "a block of a file based past the region start"},
      {CV_UF2_FAMILY_CUBEVOX, kBlock5, 256u, 5u, kFull + 1u, false, "a block of a file larger than the region"},
      {CV_UF2_FAMILY_CUBEVOX, kBlock5, 256u, 5u, 5u, false, "a block numbered past its file's count"},
      {CV_UF2_FAMILY_CUBEVOX, kBlock5, 256u, 5u, 0x01000001u, false, "a block count whose size overflows"},
  };
  char message[112];
  for (const Case& c : cases) {
    std::snprintf(message, sizeof message, "uf2: %s is %s", c.what, c.accepted ? "accepted" : "refused");
    check(cvUf2BlockAccepted(c.family, c.address, c.length, c.blockNo, c.numBlocks) == c.accepted, message);
  }
}

void checkUf2Session() {
  struct Case {
    CvUf2SessionInputs in;
    bool restart;
    const char* what;
  };
  const Case cases[] = {
      {{0u, 700u, 0u, true, true, true}, false, "the first block starts the transfer, not a restart"},
      {{700u, 700u, 5u, false, true, false}, false,
       "a block landing on the old image in a sector not yet erased joins the transfer"},
      {{700u, 700u, 5u, true, false, false}, false,
       "a block on blank or identical flash joins the transfer, as a re-sent block does"},
      {{700u, 701u, 5u, false, false, false}, true, "a file with another block count restarts"},
      {{700u, 700u, 5u, true, true, false}, true,
       "a block on other data in a sector this transfer erased is a second file and restarts"},
      {{700u, 700u, 0u, true, false, true}, true, "the first block again after a failed write restarts"},
      {{700u, 700u, 6u, true, false, true}, false,
       "the rest of the file whose write failed joins the transfer, so it never completes"},
      {{700u, 700u, 0u, true, false, false}, false, "the first block re-sent in a good transfer joins it"},
  };
  char message[128];
  for (const Case& c : cases) {
    std::snprintf(message, sizeof message, "uf2: %s", c.what);
    check(cvUf2SessionRestart(&c.in) == c.restart, message);
  }
}

void checkUpdateStalled() {
  constexpr uint32_t kLast = 1000u;
  check(!cvUpdateStalled(true, false, kLast + CV_WRITE_IDLE_MS - 1u, kLast),
        "stall: a copy 1 ms short of the idle limit is still writing");
  check(cvUpdateStalled(true, false, kLast + CV_WRITE_IDLE_MS, kLast),
        "stall: a truncated file, no block for 5 s, has stopped");
  check(!cvUpdateStalled(true, true, kLast + 60000u, kLast), "stall: a complete write never stalls");
  check(!cvUpdateStalled(false, false, kLast + 60000u, kLast),
        "stall: a host session with no accepted block never stalls");
  check(cvUpdateStalled(true, false, 0x00000388u, 0xFFFFF000u), "stall: the idle time counts across the wrap");
  check(!cvUpdateStalled(true, false, 0x00000387u, 0xFFFFF000u), "stall: and is not cut short by it");
}

// Update mode as board_hooks.c drives it, with ghostfat's write count and no USB or flash. File
// blocks are 256 bytes from CV_APP_START, so a short file sits in sector 1.
struct Bench {
  CvUpdateSession s{};
  CvBootBlock block = bootBlock();
  uint32_t now = 1000u;
  // ghostfat's WriteState: the transfer's block count and the blocks written, and whether the
  // board heard of the last one.
  uint32_t transferBlocks = 0u;
  uint32_t written = 0u;
  bool notified = false;
  bool imageGood = true;
  bool marked = false;

  Bench() {
    block.request = CV_REQUEST_UPDATE;
    const CvBootDecision d = decide(block, CvImageValid);
    cvSessionStart(&s, d.reason, true, now);
    cvSessionCable(&s, true, now);
  }

  void writeStateReset() {
    transferBlocks = 0u;
    written = 0u;
    notified = false;
  }

  // patch 08: every mount forgets the last session's copy.
  void mount(bool mounted) {
    if (mounted) writeStateReset();
    cvSessionMount(&s, mounted);
  }

  CvBlockVerdict send(uint32_t no, uint32_t count, bool otherData = false, bool writeOk = true,
                      uint32_t family = CV_UF2_FAMILY_CUBEVOX) {
    const CvUf2BlockInfo info = {family, CV_APP_START + no * 256u, 256u, no, count, transferBlocks};
    const bool erased = cvSessionSectorErased(&s, info.targetAddr, transferBlocks);
    const CvBlockVerdict verdict = cvSessionBlock(&s, &info, erased && otherData);
    if (verdict == CvBlockRefused) return verdict;
    if (verdict == CvBlockRestarted) writeStateReset();
    cvSessionBlockWritten(&s, writeOk, now);
    if (!writeOk) return verdict;
    transferBlocks = count;
    ++written;
    if (written >= transferBlocks && !notified) {
      notified = true;
      if (cvSessionLastBlock(&s, imageGood)) {
        cvBootMarkUpdated(&block);
        marked = true;
      }
    }
    return verdict;
  }

  void sendFile(uint32_t from, uint32_t to, uint32_t count) {
    for (uint32_t no = from; no < to; ++no) send(no, count);
  }

  bool armed() const { return cvSessionTimeoutArmed(&s, &block, CvImageValid); }

  CvTickAction tick(uint32_t ms) {
    now += ms;
    return cvSessionTick(&s, armed(), now);
  }

  // The cable reading held long enough for the debounce, and what the board must do.
  CvCableAction cable(bool in) {
    cvSessionCable(&s, in, now);
    now += CV_VBUS_DEBOUNCE_MS;
    return cvSessionCable(&s, in, now);
  }

  CvScreen screen() const { return cvSessionScreen(&s, true, armed()); }
};

void checkSessionStall() {
  Bench b;
  b.mount(true);
  b.sendFile(0u, 699u, 700u);
  check(b.tick(CV_WRITE_IDLE_MS - 1u) == CvTickNone && b.screen() == CvScreenWriting,
        "session: a truncated file shows Writing until the idle limit");
  b.tick(1u);
  check(b.screen() == CvScreenWaitNoImage && !b.marked, "session: then the update stopped, the block unmarked");
  b.send(699u, 700u);
  check(b.screen() == CvScreenDone && b.marked, "session: a late last block still completes the copy");

  Bench pulled;
  pulled.mount(true);
  pulled.sendFile(0u, 10u, 700u);
  check(pulled.cable(false) == CvCableUnplugged, "session: a pull mid-copy posts the unplug");
  pulled.mount(false);
  pulled.tick(CV_WRITE_IDLE_MS);
  check(pulled.screen() == CvScreenCableOut, "session: a pulled cable keeps its screen past the idle limit");
}

void checkSessionEject() {
  Bench early;
  early.mount(true);
  early.sendFile(0u, 10u, 700u);
  cvSessionEjected(&early.s, early.now);
  check(early.tick(CV_EJECT_SETTLE_MS - 1u) == CvTickNone, "session: an eject mid-copy waits for its status");
  check(early.tick(1u) == CvTickRestart && !early.marked,
        "session: then restarts unmarked, so the next start offers the drive again");

  Bench idle;
  idle.mount(true);
  cvSessionEjected(&idle.s, idle.now);
  check(idle.tick(1000u) == CvTickNone && idle.screen() == CvScreenWaitMenu,
        "session: an eject with nothing written changes nothing");

  Bench done;
  done.mount(true);
  done.sendFile(0u, 4u, 4u);
  check(done.screen() == CvScreenDone && done.marked, "session: the last block of a good image shows Done");
  cvSessionEjected(&done.s, done.now);
  check(done.tick(CV_EJECT_SETTLE_MS - 1u) == CvTickNone && done.tick(1u) == CvTickRestart,
        "session: an eject after Done restarts 20 ms later");

  Bench cable;
  cable.mount(true);
  cable.sendFile(0u, 4u, 4u);
  check(cable.cable(false) == CvCableRestart, "session: a pull after Done restarts into the new image");
}

void checkSessionAfterDone() {
  Bench second;
  second.mount(true);
  second.sendFile(0u, 4u, 4u);
  check(second.send(0u, 4u, true) == CvBlockRestarted && second.screen() == CvScreenWriting,
        "session: a second file of the same count on other data restarts and clears Done");

  Bench refused;
  refused.mount(true);
  refused.sendFile(0u, 4u, 4u);
  check(refused.send(0u, 4u, false, true, 0x6DB66082u) == CvBlockRefused && refused.screen() == CvScreenDone,
        "session: a refused block after Done keeps Done");
}

void checkSessionFailures() {
  Bench failed;
  failed.mount(true);
  failed.sendFile(0u, 4u, 700u);
  failed.send(4u, 700u, false, false);
  check(failed.screen() == CvScreenWaitNoImage, "session: a failed write stops the update");
  check(failed.send(5u, 700u) == CvBlockJoined && failed.screen() == CvScreenWaitNoImage,
        "session: the rest of that file joins and the update stays stopped");
  check(failed.send(0u, 700u) == CvBlockRestarted && failed.screen() == CvScreenWriting && failed.written == 1u,
        "session: the same file again restarts on fresh erases");

  Bench corrupt;
  corrupt.imageGood = false;
  corrupt.mount(true);
  corrupt.sendFile(0u, 4u, 4u);
  check(!corrupt.marked && corrupt.screen() == CvScreenWaitNoImage,
        "session: a complete file that fails the image check stops the update, the block unmarked");
  corrupt.imageGood = true;
  check(corrupt.send(0u, 4u) == CvBlockRestarted, "session: and the file dropped again restarts");
  corrupt.sendFile(1u, 4u, 4u);
  check(corrupt.marked && corrupt.screen() == CvScreenDone, "session: a good copy then completes");

  Bench wrong;
  wrong.mount(true);
  cvSessionWrongFile(&wrong.s, true);
  check(wrong.screen() == CvScreenWrongFile && wrong.s.diagDirty, "session: a wrong file name shows the wrong file");
  wrong.send(0u, 700u);
  check(wrong.screen() == CvScreenWriting, "session: then a good file writes");
}

void checkSessionTimeout() {
  Bench before;
  check(before.armed(), "session: a menu entry with no computer counts down");
  before.mount(true);
  check(!before.armed() && before.screen() == CvScreenWaitMenu, "session: a host stops the countdown");
  check(before.cable(false) == CvCableUnplugged, "session: the pull posts the unplug");
  before.mount(false);
  check(before.armed() && before.tick(CV_UPDATE_TIMEOUT_MS - 1u) == CvTickNone,
        "session: a pull with nothing written counts 60 s again");
  check(before.tick(1u) == CvTickTimeout, "session: from the pull");

  Bench after;
  after.mount(true);
  after.send(0u, 700u);
  after.cable(false);
  after.mount(false);
  check(!after.armed() && after.tick(CV_UPDATE_TIMEOUT_MS) == CvTickNone && after.screen() == CvScreenCableOut,
        "session: a pull after a write never times out and shows the cable");
  check(after.cable(true) == CvCableConnect && after.screen() == CvScreenWaitNoImage,
        "session: the cable back shows that the update stopped");

  CvBootDecision d = {CvActionUpdateMode, CvReasonImageUnreadable, 0x08040000u, true};
  check(cvRegionReadAllowed(&d, false) && !cvRegionReadAllowed(&d, true),
        "session: the region read is skipped when the erase failed");
  d.imageReadable = false;
  check(!cvRegionReadAllowed(&d, false), "session: and when the decision says the image is unreadable");
}

// A root directory sector as the host writes it.
struct DirSector {
  uint8_t bytes[512] = {};
  uint32_t used = 0;

  void add(const char* name11, uint8_t attr) {
    uint8_t* entry = &bytes[used * 32u];
    std::memcpy(entry, name11, 11);
    entry[11] = attr;
    ++used;
  }
};

void checkDirScan() {
  DirSector finder;
  finder.add("CUBEVOX    ", 0x08);
  finder.add("INFO_UF2TXT", 0x01);
  finder.add("DIAG    TXT", 0x01);
  finder.add("A\0.\0f\0s\0e\0", 0x0F);
  finder.add("FSEVEN~1   ", 0x12);
  finder.add("SPOTLI~1   ", 0x12);
  finder.add("DS_STO~1   ", 0x02);
  finder.add("_~1     TRA", 0x02);
  finder.add("CUBEVO~1UF2", 0x20);
  finder.add("_CUBEV~1UF2", 0x22);
  char name[13] = "untouched";
  check(!cvDirNamesWrongFile(finder.bytes, name) && std::strcmp(name, "untouched") == 0,
        "dir: Finder's own files, the drive's files and a .uf2 name no wrong file");

  DirSector rom = finder;
  rom.add("CUBEVO~2BIN", 0x20);
  check(cvDirNamesWrongFile(rom.bytes, name) && std::strcmp(name, "CUBEVO~2.BIN") == 0,
        "dir: the release's ROM .bin is a wrong file, named for DIAG.TXT");

  DirSector source;
  source.add("SOURCE~1GZ ", 0x20);
  check(cvDirNamesWrongFile(source.bytes, name) && std::strcmp(name, "SOURCE~1.GZ") == 0,
        "dir: a source archive is a wrong file");
}

// Entries the scan must pass over even with a wrong extension.
void checkDirScanSkips() {
  char name[13];
  DirSector deleted;
  deleted.add("\xE5UBEVO~2BIN", 0x20);
  check(!cvDirNamesWrongFile(deleted.bytes, name), "dir: a deleted entry is not a file");

  DirSector ended;
  ended.add("CUBEVOX    ", 0x08);
  ended.used += 1;  // a free entry ends the directory
  ended.add("CUBEVO~2BIN", 0x20);
  check(!cvDirNamesWrongFile(ended.bytes, name), "dir: entries after the end marker are not read");

  DirSector folder;
  folder.add("FIRMWAREBIN", 0x10);
  check(!cvDirNamesWrongFile(folder.bytes, name), "dir: a folder is not a file");
}

struct ScreenCase {
  CvScreenInputs in;
  CvScreen screen;
  const char* what;
};

// reason, imageValid, writeBegun, accepted, refused, failed, cableOut, timeoutArmed, bootUpdated
const ScreenCase kScreenCases[] = {
      {{CvReasonImageInvalid, false, false, false, false, false, false, false, true}, CvScreenBootUpdated,
       "no image after a bootloader update"},
      {{CvReasonAppRequest, false, false, false, false, false, false, false, true}, CvScreenBootUpdated,
       "a request with no image after a bootloader update"},
      {{CvReasonAppRequest, true, false, false, false, false, false, true, true}, CvScreenWaitMenu,
       "the bootloader-updated mark with an image installed changes nothing"},
      {{CvReasonBootFailed, true, false, false, false, false, false, false, true}, CvScreenWaitBootFailed,
       "the mark leaves three failed starts as they are"},
      {{CvReasonImageInvalid, false, true, true, false, false, false, false, true}, CvScreenWriting,
       "the firmware arriving after a bootloader update"},
      {{CvReasonImageInvalid, false, true, true, false, true, false, false, true}, CvScreenWaitNoImage,
       "a firmware write that fails after a bootloader update stops the update"},
      {{CvReasonImageInvalid, false, true, false, false, false, false, false, true}, CvScreenWaitNoImage,
       "a firmware copy cut short after a bootloader update"},
      {{CvReasonImageInvalid, false, false, false, false, false, true, false, true}, CvScreenCableOut,
       "a cable pulled after a bootloader update"},
      {{CvReasonClockFailed, false, false, false, false, false, false, false, true}, CvScreenClockFailed,
       "a failed clock after a bootloader update"},
      {{CvReasonAppRequest, true, false, false, false, false, false, true, false}, CvScreenWaitMenu, "menu entry"},
      {{CvReasonAppRequest, false, false, false, false, false, false, false, false}, CvScreenWaitNoImage,
       "a request with no valid image"},
      {{CvReasonBootFault, true, false, false, false, false, false, true, false}, CvScreenWaitMenu,
       "a bootloader fault with a valid image"},
      {{CvReasonBootFailed, true, false, false, false, false, false, false, false}, CvScreenWaitBootFailed,
       "three failed starts"},
      {{CvReasonImageInvalid, false, false, false, false, false, false, false, false}, CvScreenWaitNoImage,
       "an invalid image"},
      {{CvReasonClockFailed, true, false, false, false, false, false, false, false}, CvScreenClockFailed,
       "a failed clock"},
      {{CvReasonAppRequest, true, true, true, false, false, false, false, false}, CvScreenWriting,
       "blocks arriving"},
      {{CvReasonAppRequest, true, false, false, true, false, false, true, false}, CvScreenWrongFile,
       "refused blocks"},
      {{CvReasonAppRequest, true, true, true, true, true, false, false, false}, CvScreenWaitNoImage,
       "a refusal after accepted blocks stops the update"},
      {{CvReasonAppRequest, true, true, true, false, true, false, false, false}, CvScreenWaitNoImage,
       "a failed write stops the update"},
      {{CvReasonAppRequest, true, true, false, false, false, true, false, false}, CvScreenCableOut,
       "a cable pulled mid-copy"},
      {{CvReasonImageInvalid, false, false, false, false, false, true, false, false}, CvScreenCableOut,
       "a cable pulled with no image to return to"},
      {{CvReasonAppRequest, true, false, false, false, false, true, true, false}, CvScreenWaitMenu,
       "a cable pulled before any write counts down again"},
      {{CvReasonAppRequest, true, true, false, false, false, false, false, false}, CvScreenWaitNoImage,
       "a copy cut short with the cable back"},
};

void checkUpdateScreens() {
  char message[112];
  for (const ScreenCase& c : kScreenCases) {
    std::snprintf(message, sizeof message, "screen: %s", c.what);
    check(cvUpdateScreen(&c.in) == c.screen, message);
  }
}

void checkDebounce() {
  CvDebounce vbus;
  cvDebounceReset(&vbus, true);
  check(!cvDebounceUpdate(&vbus, false, 1000u, 50u) && vbus.level, "debounce: a drop starts the hold");
  check(!cvDebounceUpdate(&vbus, true, 1020u, 50u) && vbus.level, "debounce: a 20 ms glitch is ignored");
  check(!cvDebounceUpdate(&vbus, false, 1030u, 50u), "debounce: a new drop starts a new hold");
  check(!cvDebounceUpdate(&vbus, false, 1079u, 50u) && vbus.level, "debounce: 49 ms is not enough");
  check(cvDebounceUpdate(&vbus, false, 1080u, 50u) && !vbus.level, "debounce: 50 ms changes the level once");
  check(!cvDebounceUpdate(&vbus, false, 2000u, 50u), "debounce: a steady level reports no change");

  cvDebounceReset(&vbus, false);
  check(!cvDebounceUpdate(&vbus, true, 0xFFFFFFF0u, 50u), "debounce: a hold may start just before the wrap");
  check(cvDebounceUpdate(&vbus, true, 0x00000030u, 50u) && vbus.level, "debounce: and end after it");
}

void checkImageHeaderVersion() {
  check(!cvImageHeaderVersionOk(0u), "image: header version 0 is no header");
  check(cvImageHeaderVersionOk(1u) && cvImageHeaderVersionOk(2u) && cvImageHeaderVersionOk(0xFFFFFFFFu),
        "image: version 1 and every later one read as version 1");
}

struct NameRow {
  uint32_t value;
  const char* text;
};

// Checks each row of one name function. The value after the last row and two garbage words must
// all read `other`, because DTCM holds whatever the last run or power-on left.
template <std::size_t N>
void checkNameTable(const char* table, const char* (*nameOf)(uint32_t), const NameRow (&rows)[N],
                    const char* other) {
  char message[96];
  for (const NameRow& row : rows) {
    std::snprintf(message, sizeof message, "names: %s %u reads %s", table,
                  static_cast<unsigned>(row.value), row.text);
    check(std::strcmp(nameOf(row.value), row.text) == 0, message);
  }
  const uint32_t unknown[] = {rows[N - 1].value + 1u, 0xDEADBEEFu, 0xFFFFFFFFu};
  for (const uint32_t value : unknown) {
    std::snprintf(message, sizeof message, "names: %s 0x%X reads %s", table,
                  static_cast<unsigned>(value), other);
    check(std::strcmp(nameOf(value), other) == 0, message);
  }
}

// The strings faultReport, faultSummaryText and DIAG.TXT print, which one definition serves.
void checkBootNames() {
  const NameRow reasons[] = {{CvReasonNone, "none"},
                             {CvReasonMenuHeld, "MenuHeld"},
                             {CvReasonAppRequest, "AppRequest"},
                             {CvReasonImageInvalid, "ImageInvalid"},
                             {CvReasonImageUnreadable, "ImageUnreadable"},
                             {CvReasonBootFailed, "BootFailed"},
                             {CvReasonBootFault, "BootFault"},
                             {CvReasonClockFailed, "ClockFailed"}};
  const NameRow kinds[] = {{CvFaultKindHard, "hard"},
                           {CvFaultKindMemManage, "memmanage"},
                           {CvFaultKindBus, "bus"},
                           {CvFaultKindUsage, "usage"},
                           {CvFaultKindClock, "clock"}};
  const NameRow steps[] = {{0, "?"},
                           {CvClockStepSupply, "supply"},
                           {CvClockStepVoltageScale, "voltage scaling"},
                           {CvClockStepOscillator, "oscillator and PLL1"},
                           {CvClockStepClockTree, "bus clocks"},
                           {CvClockStepPeripheralClocks, "peripheral clocks"}};
  const NameRow options[] = {{CvOptionUnchecked, "unchecked"},
                             {CvOptionAlreadySet, "set"},
                             {CvOptionProgrammed, "programmed"},
                             {CvOptionLocked, "failed, locked"},
                             {CvOptionTimeout, "failed, timeout"},
                             {CvOptionChangeError, "failed, change error"},
                             {CvOptionNotApplied, "failed, not applied"},
                             {CvOptionInterrupted, "failed, interrupted"}};
  checkNameTable("reason", cvBootReasonName, reasons, "unknown");
  checkNameTable("fault kind", cvFaultKindName, kinds, "unknown");
  checkNameTable("clock step", cvClockStepName, steps, "?");
  checkNameTable("option result", cvOptionResultName, options, "unknown");
  check(cvBootReasonKnown(CvReasonClockFailed) && !cvBootReasonKnown(CvReasonClockFailed + 1u),
        "names: the known reasons end at ClockFailed");
}

}  // namespace

void checkBootDecide() {
  checkBootNames();
  checkImageHeaderVersion();
  checkBootRows();
  checkBootPriority();
  checkBootNoMenuReason();
  checkBootCounter();
  checkBootPowerOnFlags();
  checkBootPowerOn();
  checkBootRestartsCount();
  checkBootGarbage();
  checkBootJunkAfterRomSession();
  checkBootSeal();
  checkBootResetFlags();
  checkBootStaleFields();
  checkBootVersions();
  checkBootReadFault();
  checkBootReadFaultAfter();
  checkBootBank2Fault();
  checkBootFaultRegisters();
  checkBootFaultRepeats();
  checkBootClock();
  checkBootTimeout();
  checkBootMarkUpdated();
  checkBootUpdatedMark();
  checkOptionBytes();
  checkUf2Filter();
  checkUf2Session();
  checkUpdateStalled();
  checkSessionStall();
  checkSessionEject();
  checkSessionAfterDone();
  checkSessionFailures();
  checkSessionTimeout();
  checkDirScan();
  checkDirScanSkips();
  checkUpdateScreens();
  checkDebounce();
}
