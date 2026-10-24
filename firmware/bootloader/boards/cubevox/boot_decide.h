// The bootloader's policy as pure C: run the application or stay in update mode. No HAL, no I/O.
// Board code reads the MENU switch, checks the image, calls these functions and acts on the
// result. The host tests (test/firmware_test.cpp) compile boot_decide.c unchanged.
//
// Row numbers refer to the Boot sequence table of docs/specs/2026-10-24-field-update-design.md.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../../../cubevox/src/board/boot_block.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CvImageValid, CvImageInvalid } CvImageStatus;
typedef enum { CvActionRunApp, CvActionUpdateMode } CvBootAction;

typedef struct {
  CvBootAction action;
  // Why update mode, or CvReasonNone when the application runs.
  enum CvBootReason reason;
  // Application sector to erase before enumerating, 0 if none.
  uint32_t eraseAddress;
} CvBootDecision;

// Evaluates the decision table top to bottom and updates the block.
//
// Update mode stores the reason in lastReason. Running the application adds one to bootFails and
// writes its check word. The request word is cleared in every case and blockVersion is written.
//
// A request whose lastReason is CvReasonImageUnreadable is a read fault recorded by
// cvBootRecordReadFault (row 4). It yields the 128 KB sector holding faultAddress when that
// address lies in the application region, whichever row wins, so the unreadable word is erased
// before anything reads it again. Outside the region there is nothing to erase and the reason
// becomes CvReasonImageInvalid.
//
// A pending request outranks the image, so board code can pass CvImageInvalid without reading
// flash when block->request is set. Reading would fault on the very word the request names.
CvBootDecision cvBootDecide(struct CvBootBlock* block, bool menuHeld, CvImageStatus image);

// Fault handler path. Records the unreadable flash address and asks for update mode. The caller
// resets afterwards.
void cvBootRecordReadFault(struct CvBootBlock* block, uint32_t flashAddress);

// True when update mode may fall back to the application after CV_UPDATE_TIMEOUT_MS. That holds
// when the reason is MenuHeld or AppRequest, the image is valid, no host has enumerated and no
// write has begun.
bool cvBootTimeoutAllowed(const struct CvBootBlock* block, CvImageStatus image, bool hostEnumerated,
                          bool writeBegun);

// After a complete UF2 write. Zeroes the counter and clears the request, the reason, the fault
// address and the application's summary.
void cvBootMarkUpdated(struct CvBootBlock* block);

#ifdef __cplusplus
}
#endif
