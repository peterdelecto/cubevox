// Checks the bootloader on the host. Covers the boot block contract and the decision policy,
// which the host build compiles from boot_decide.c as C99.

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
static_assert(CV_BOOT_BLOCK_ADDR + offsetof(CvBootBlock, reserved) == 0x2001FF40u, "boot: reserved");
static_assert(sizeof(CvBootBlock::appSummary) == 40 && sizeof(CvBootBlock::reserved) == 128,
              "boot: appSummary is 40 bytes and reserved 128");
static_assert(sizeof(CvImageHeader) == 32, "boot: the image header is 32 bytes");
static_assert(offsetof(CvImageHeader, magic) == 0 && offsetof(CvImageHeader, headerVersion) == 4 &&
                  offsetof(CvImageHeader, length) == 8 && offsetof(CvImageHeader, crc32) == 12 &&
                  offsetof(CvImageHeader, version) == 16 && sizeof(CvImageHeader::version) == 16,
              "boot: image header offsets");
static_assert(CV_REQUEST_UPDATE == 0xF01669EFu, "boot: the request word is TinyUF2's double-tap magic");
static_assert(CV_IMAGE_MAGIC == (uint32_t{'C'} | uint32_t{'V'} << 8 | uint32_t{'F'} << 16 | uint32_t{'W'} << 24),
              "boot: the image magic spells CVFW in file order");
static_assert(CV_UF2_FAMILY_CUBEVOX != CV_UF2_FAMILY_WEACT && CV_UF2_FAMILY_CUBEVOX != 0x6db66082u &&
                  CV_UF2_FAMILY_WEACT != 0x6db66082u,
              "boot: the family ids differ from each other and from the generic STM32H7 id");
static_assert(CV_APP_START == 0x08020000u && CV_APP_END == 0x08100000u && CV_IMAGE_HEADER_OFFSET == 0x400u,
              "boot: flash map");
static_assert(CV_USB_VID_BOOT == 0x1209u && CV_USB_PID_BOOT == 0x0001u && CV_USB_VID_APP == 0x0483u &&
                  CV_USB_PID_APP == 0x5740u,
              "boot: USB ids");
static_assert(CV_BOOT_FAIL_LIMIT == 3u && CV_UPDATE_TIMEOUT_MS == 60000u && CV_HEALTHY_MS == 5000u,
              "boot: limits");

namespace {

// A block as the bootloader leaves it after a healthy start: version set, counter 0 with its check.
CvBootBlock bootBlock() {
  CvBootBlock b;
  std::memset(&b, 0, sizeof b);
  b.blockVersion = CV_BOOT_BLOCK_VERSION;
  cvBootFailsSet(&b, 0);
  return b;
}

bool decided(const CvBootDecision& d, CvBootAction action, CvBootReason reason, uint32_t erase = 0) {
  return d.action == action && d.reason == reason && d.eraseAddress == erase;
}

// Runs a decision and checks the two writes every decision makes.
CvBootDecision decide(CvBootBlock& b, bool menuHeld, CvImageStatus image) {
  const CvBootDecision d = cvBootDecide(&b, menuHeld, image);
  check(b.request == 0, "boot: a decision consumes the request word");
  check(b.blockVersion == CV_BOOT_BLOCK_VERSION, "boot: a decision writes the block version");
  return d;
}

// One block per row of the decision table, set up so that only that row applies.
void checkBootRows() {
  CvBootBlock b = bootBlock();
  check(decided(decide(b, true, CvImageValid), CvActionUpdateMode, CvReasonMenuHeld), "boot: row 1, MENU held");
  check(b.lastReason == CvReasonMenuHeld && cvBootFailsGet(&b) == 0,
        "boot: row 1 stores the reason and leaves the counter");

  b = bootBlock();
  b.request = CV_REQUEST_UPDATE;
  check(decided(decide(b, false, CvImageValid), CvActionUpdateMode, CvReasonAppRequest),
        "boot: row 2, request from the application");
  check(b.lastReason == CvReasonAppRequest, "boot: row 2 stores the reason");

  b = bootBlock();
  check(decided(decide(b, false, CvImageInvalid), CvActionUpdateMode, CvReasonImageInvalid),
        "boot: row 3, invalid image");
  check(b.lastReason == CvReasonImageInvalid, "boot: row 3 stores the reason");

  b = bootBlock();
  cvBootRecordReadFault(&b, 0x08065432u);
  check(decided(decide(b, false, CvImageValid), CvActionUpdateMode, CvReasonImageUnreadable, 0x08060000u),
        "boot: row 4, read fault yields its sector");
  check(b.lastReason == CvReasonImageUnreadable && b.faultAddress == 0x08065432u,
        "boot: row 4 keeps the reason and the address for DIAG.TXT");

  b = bootBlock();
  cvBootFailsSet(&b, CV_BOOT_FAIL_LIMIT);
  check(decided(decide(b, false, CvImageValid), CvActionUpdateMode, CvReasonBootFailed),
        "boot: row 5, three failed starts");
  check(cvBootFailsGet(&b) == CV_BOOT_FAIL_LIMIT && b.lastReason == CvReasonBootFailed,
        "boot: row 5 stores the reason and leaves the counter");

  b = bootBlock();
  check(decided(decide(b, false, CvImageValid), CvActionRunApp, CvReasonNone), "boot: row 6, run the application");
  check(cvBootFailsGet(&b) == 1 && cvBootFailsValid(&b), "boot: row 6 counts the start and writes the check word");
}

// Several rows apply at once and the higher row wins.
void checkBootPriority() {
  CvBootBlock b = bootBlock();
  b.request = CV_REQUEST_UPDATE;
  cvBootFailsSet(&b, CV_BOOT_FAIL_LIMIT);
  check(decided(decide(b, true, CvImageInvalid), CvActionUpdateMode, CvReasonMenuHeld),
        "boot: MENU held beats a request, an invalid image and a failed count");

  b.request = CV_REQUEST_UPDATE;
  check(decided(decide(b, false, CvImageInvalid), CvActionUpdateMode, CvReasonAppRequest),
        "boot: a request beats an invalid image and a failed count");
  check(decided(decide(b, false, CvImageInvalid), CvActionUpdateMode, CvReasonImageInvalid),
        "boot: an invalid image beats a failed count");
  check(decided(decide(b, false, CvImageValid), CvActionUpdateMode, CvReasonBootFailed),
        "boot: a failed count alone stays in update mode");

  b = bootBlock();
  cvBootRecordReadFault(&b, 0x08065432u);
  cvBootFailsSet(&b, CV_BOOT_FAIL_LIMIT);
  check(decided(decide(b, false, CvImageInvalid), CvActionUpdateMode, CvReasonImageUnreadable, 0x08060000u),
        "boot: a pending read fault beats an invalid image and a failed count");

  b = bootBlock();
  cvBootRecordReadFault(&b, 0x08065432u);
  check(decided(decide(b, true, CvImageValid), CvActionUpdateMode, CvReasonMenuHeld, 0x08060000u),
        "boot: MENU held names the reason but a pending read fault still yields its sector");
}

void checkBootCounter() {
  CvBootBlock b = bootBlock();
  b.bootFails = 7;
  b.bootFailsCheck = 0x12345678u;
  check(!cvBootFailsValid(&b) && cvBootFailsGet(&b) == 0, "boot: a counter whose check word disagrees reads as 0");
  check(decided(decide(b, false, CvImageValid), CvActionRunApp, CvReasonNone) && cvBootFailsGet(&b) == 1 &&
            cvBootFailsValid(&b),
        "boot: with a garbage check word the next start counts as the first");

  b = bootBlock();
  for (uint32_t n = 1; n <= CV_BOOT_FAIL_LIMIT; ++n) {
    const CvBootDecision d = decide(b, false, CvImageValid);
    check(d.action == CvActionRunApp && cvBootFailsGet(&b) == n, "boot: starts below the limit run and count");
  }
  for (int i = 0; i < 3; ++i) {
    check(decided(decide(b, false, CvImageValid), CvActionUpdateMode, CvReasonBootFailed) &&
              cvBootFailsGet(&b) == CV_BOOT_FAIL_LIMIT,
          "boot: at the limit update mode stays and the counter stays put");
  }

  b = bootBlock();
  cvBootFailsSet(&b, 0xFFFFFFFFu);
  check(decided(decide(b, false, CvImageValid), CvActionUpdateMode, CvReasonBootFailed) &&
            cvBootFailsGet(&b) == 0xFFFFFFFFu,
        "boot: a huge valid count stays in update mode without wrapping");
}

// DTCM is random after power-on. Whatever the bytes, the first start is a first start and the
// diagnostic fields come out clean.
void checkBootGarbage() {
  const uint8_t fills[] = {0x00, 0xA5, 0xFF};
  for (const uint8_t fill : fills) {
    CvBootBlock b;
    std::memset(&b, fill, sizeof b);
    check(decided(decide(b, false, CvImageValid), CvActionRunApp, CvReasonNone) && cvBootFailsGet(&b) == 1,
          "boot: a garbage block gives a first start");
    bool clean = b.lastReason == CvReasonNone && b.faultAddress == 0;
    for (const char ch : b.appSummary) clean = clean && ch == '\0';
    for (const uint8_t byte : b.reserved) clean = clean && byte == 0;
    check(clean, "boot: a garbage block comes out with no reason, fault address, summary or reserved bytes");
  }

  CvBootBlock b = bootBlock();
  b.lastReason = 0xDEADBEEFu;
  decide(b, false, CvImageValid);
  check(b.lastReason == CvReasonNone, "boot: an out-of-range reason is cleared, it indexes name tables");

  b = bootBlock();
  b.lastReason = 0xDEADBEEFu;
  b.request = CV_REQUEST_UPDATE;
  check(decided(decide(b, false, CvImageValid), CvActionUpdateMode, CvReasonAppRequest),
        "boot: a request with a garbage reason is a plain request");

  b = bootBlock();
  std::strcpy(b.appSummary, "v2.00.10 up 5 s");
  b.request = CV_REQUEST_UPDATE;
  decide(b, false, CvImageValid);
  check(std::strcmp(b.appSummary, "v2.00.10 up 5 s") == 0, "boot: a decision keeps the application's summary");
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
      {0x08065432u, 0x08060000u, CvReasonImageUnreadable, "an address in sector 3"},
      {0x08020000u, 0x08020000u, CvReasonImageUnreadable, "the first application word"},
      {0x080FFFFCu, 0x080E0000u, CvReasonImageUnreadable, "the last application word"},
      {0x0801FFFCu, 0u, CvReasonImageInvalid, "the bootloader sector, which is never erased"},
      {0x08100000u, 0u, CvReasonImageInvalid, "the first word of bank 2"},
      {0x08145678u, 0u, CvReasonImageInvalid, "an address in bank 2"},
      {0u, 0u, CvReasonImageInvalid, "address zero"},
      {0xFFFFFFFFu, 0u, CvReasonImageInvalid, "a garbage address"},
  };
  char message[96];
  for (const Case& c : cases) {
    CvBootBlock b = bootBlock();
    cvBootRecordReadFault(&b, c.address);
    check(b.request == CV_REQUEST_UPDATE && b.lastReason == CvReasonImageUnreadable && b.faultAddress == c.address,
          "boot: recording a read fault sets the request, the reason and the address");
    std::snprintf(message, sizeof message, "boot: read fault at %s", c.what);
    check(decided(decide(b, false, CvImageInvalid), CvActionUpdateMode, c.reason, c.erase), message);
    check(b.faultAddress == c.address, "boot: the fault address stays for DIAG.TXT");
  }

  CvBootBlock b = bootBlock();
  cvBootRecordReadFault(&b, 0x08065432u);
  decide(b, false, CvImageInvalid);
  check(decided(decide(b, false, CvImageInvalid), CvActionUpdateMode, CvReasonImageInvalid) && b.faultAddress == 0,
        "boot: the sector is yielded once, a later start sees a plain invalid image");

  CvBootBlock garbage;
  std::memset(&garbage, 0xA5, sizeof garbage);
  cvBootRecordReadFault(&garbage, 0x08065432u);
  check(decided(decide(garbage, false, CvImageInvalid), CvActionUpdateMode, CvReasonImageUnreadable, 0x08060000u),
        "boot: a read fault recorded on a power-on garbage block survives to the decision");
}

void checkBootTimeout() {
  int wrong = 0;
  for (uint32_t reason = CvReasonNone; reason <= CvReasonBootFailed; ++reason) {
    for (int mask = 0; mask < 8; ++mask) {
      const CvImageStatus image = (mask & 1) ? CvImageInvalid : CvImageValid;
      const bool host = (mask & 2) != 0;
      const bool written = (mask & 4) != 0;
      CvBootBlock b = bootBlock();
      b.lastReason = reason;
      const bool menuOrApp = reason == CvReasonMenuHeld || reason == CvReasonAppRequest;
      const bool want = menuOrApp && image == CvImageValid && !host && !written;
      if (cvBootTimeoutAllowed(&b, image, host, written) != want) ++wrong;
    }
  }
  check(wrong == 0, "boot: the timeout needs MenuHeld or AppRequest, a valid image, no host and no write");

  CvBootBlock b = bootBlock();
  decide(b, true, CvImageValid);
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
  b.faultAddress = 0x08065432u;
  std::strcpy(b.appSummary, "v2.00.09 up 812 s");
  cvBootMarkUpdated(&b);
  check(cvBootFailsGet(&b) == 0 && cvBootFailsValid(&b), "boot: an update zeroes the counter with its check word");
  check(b.request == 0 && b.lastReason == CvReasonNone && b.faultAddress == 0,
        "boot: an update clears the request, the reason and the fault address");
  check(b.appSummary[0] == '\0' && b.blockVersion == CV_BOOT_BLOCK_VERSION,
        "boot: an update clears the old application's summary");
  check(decided(decide(b, false, CvImageValid), CvActionRunApp, CvReasonNone),
        "boot: after an update the next start runs the application");
}

}  // namespace

void checkBootDecide() {
  checkBootRows();
  checkBootPriority();
  checkBootCounter();
  checkBootGarbage();
  checkBootReadFault();
  checkBootTimeout();
  checkBootMarkUpdated();
}
