// The bootloader's policy as pure C: run the application or stay in update mode, what a fault
// leaves for the next start, which UF2 blocks update mode takes, how update mode follows the cable,
// the host and the transfer, and what the panel shows. No HAL,
// no I/O. Board code reads the hardware, calls these functions and acts on the result. The host
// tests (test/firmware/boot_decide.cpp) compile boot_decide.c unchanged.
//
// Row numbers refer to the Boot sequence table of docs/specs/2026-10-24-field-update-design.md.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../../../cubevox/src/board/boot_block.h"
#include "screens.h"

#ifdef __cplusplus
extern "C" {
#endif

// RM0433 register fields the policy reads. board_hooks.c checks each against CMSIS.
#define CV_RSR_BORRSTF         (1u << 21)
#define CV_RSR_PORRSTF         (1u << 23)
#define CV_RSR_SFTRSTF         (1u << 24)
#define CV_RSR_IWDG1RSTF       (1u << 26)
#define CV_RSR_WWDG1RSTF       (1u << 28)

typedef enum { CvImageValid, CvImageInvalid } CvImageStatus;
typedef enum { CvActionRunApp, CvActionUpdateMode } CvBootAction;

typedef struct {
  bool menuHeld;
  CvImageStatus image;
  // The CvClockStep that failed at this start, 0 when the clocks run as designed.
  uint32_t clockStep;
} CvBootInputs;

typedef struct {
  CvBootAction action;
  // Why update mode, or CvReasonNone when the application runs.
  enum CvBootReason reason;
  // Application sector to erase before enumerating, 0 if none.
  uint32_t eraseAddress;
  // False when the image must not be read again this start: a read fault named no sector to erase.
  bool imageReadable;
} CvBootDecision;

// What the fault handler knows when it records a fault.
typedef struct {
  // RCC_RSR, for a block cvBootAdopt has not seen yet this start.
  uint32_t resetFlags;
  bool blockAdopted;
  // Flash address of a double ECC error, 0 for any other fault.
  uint32_t eccAddress;
  // The fault hit while board code read the application image.
  bool imageReading;
  bool updateMode;
  // The stacked pc and SCB->CFSR, HFSR and BFAR.
  uint32_t pc;
  uint32_t cfsr;
  uint32_t hfsr;
  uint32_t bfar;
} CvFaultInfo;

// What the update-mode screen depends on.
typedef struct {
  // The decision's reason, an enum CvBootReason.
  uint32_t reason;
  bool imageValid;
  // Update mode has accepted a UF2 block, so the installed image is partly overwritten.
  bool writeBegun;
  // Since the host last mounted the drive, in order: blocks accepted; blocks refused or a file
  // that cannot be firmware named, before any block went in; a write failed or a block refused
  // after accepted ones.
  bool sessionAccepted;
  bool sessionRefused;
  bool sessionFailed;
  // The cable came out after a host had connected, and is still out.
  bool cableOut;
  // cvBootTimeoutAllowed holds, so the waiting screen counts down.
  bool timeoutArmed;
} CvScreenInputs;

// A level that follows a raw input only after the input holds steady.
typedef struct {
  bool level;
  bool pending;
  uint32_t sinceMs;
} CvDebounce;

// ---- Boot decision ------------------------------------------------------------------------

// True for a pure power-on: PORRSTF or BORRSTF set, and none of the software, independent
// watchdog and window watchdog flags. The flags are those of this start alone, because the
// bootloader clears RCC_RSR once it has read it. A watchdog reset after a hang counts as a start
// like any other.
bool cvBootPowerOn(uint32_t resetFlags);

// The first call of every start, before anything reads the block. A pure power-on or a block
// without the seal (cvBootBlockSealed) is cleared whole: DTCM holds nothing a reader may trust
// then, because the ST ROM loader and an older bootloader's block both fail the check. Otherwise
// the block is kept, the reason is range checked and the version and seal are written. The flags
// go into resetFlags for the application, which finds RCC_RSR cleared by the jump.
void cvBootAdopt(struct CvBootBlock* block, uint32_t resetFlags);

// True when a request stands that the decision will honour. Board code then passes CvImageInvalid
// without reading flash, because a request may name the very word that faulted.
bool cvBootRequestPending(const struct CvBootBlock* block);

// Evaluates the decision table top to bottom and updates the block. Clock failure first, then
// MENU, a pending read fault, the bootloader's own resume request, the application's request, the
// image, and the start counter.
//
// Update mode stores the reason in lastReason. Running the application adds one to bootFails and
// writes its check word. The request word is cleared in every case.
//
// A resume request whose lastReason is CvReasonImageUnreadable is a read fault (row 4). It yields
// the 128 KB sector holding faultAddress when that address lies in the application region,
// whichever row wins, so the unreadable word is erased before anything reads it again. With no
// sector to erase the reason becomes CvReasonImageInvalid and the image is not read again.
//
// A faultAddress in bank 2 is a fault in the diagnostic log. The block keeps it across starts, for
// the DIAG.TXT reader and for the application, which erases that sector and clears it. Any other
// faultAddress is cleared by the next decision.
CvBootDecision cvBootDecide(struct CvBootBlock* block, const CvBootInputs* in);

// The fault handler's record for the next start, which the caller then resets into. Every fault
// keeps its registers. A double ECC error in the application region, or any fault while the image
// is read, asks the next start for update mode with CvReasonImageUnreadable. One in bank 2 can only
// come from reading the diagnostic log: it keeps the address, and in update mode asks to resume
// with the reason unchanged. Any other fault asks for update mode with CvReasonBootFault, except a
// second one inside the update mode a first one started: the next start then decides afresh, so a
// bootloader fault that repeats cannot keep a valid image from running.
void cvBootRecordFault(struct CvBootBlock* block, const CvFaultInfo* fault);

// True when update mode may fall back to the application after CV_UPDATE_TIMEOUT_MS. That holds
// when the reason is MenuHeld, AppRequest or BootFault, the image is valid, no host has enumerated
// since the cable went in, and no write has begun.
bool cvBootTimeoutAllowed(const struct CvBootBlock* block, CvImageStatus image, bool hostSeen,
                          bool writeBegun);

// After a complete UF2 write. Zeroes the counter and clears the request, the reason, the
// bootloader's fault registers and the application's summary. A fault address in bank 2 stays for
// the new application, which erases that log sector and clears it; any other is cleared.
void cvBootMarkUpdated(struct CvBootBlock* block);

// ---- Option bytes -------------------------------------------------------------------------

// True when BOR_LEV is below 3 or bank 1 sector 0 is writable, from OPTSR_CUR and WPSN_CUR1.
bool cvOptionsWanted(uint32_t optsrCur, uint32_t wpsnCur1);

// True when the option byte step should program: wanted, and no attempt failed since power-on.
// One attempt per power-on at most, so a failure never repeats in a loop.
bool cvOptionsAttempt(const struct CvBootBlock* block, uint32_t optsrCur, uint32_t wpsnCur1);

// The values to program: OPTSR_PRG with BOR_LEV 3 and WPSN_PRG1 with sector 0 protected. Every
// other bit, the read protection level among them, keeps its value.
uint32_t cvOptionsOptsr(uint32_t optsrPrg);
uint32_t cvOptionsWpsn(uint32_t wpsnPrg1);

// ---- Update mode --------------------------------------------------------------------------

// Only this board's family, inside the application region, in whole 32-byte flash words, from a
// file that starts at CV_APP_START and fits the region. A file's blocks are numbered in address
// order with one payload size, so the block's number and address give the file's base, and its
// block count gives the file's size. Every block of a file based elsewhere or too large is refused,
// so no block of it is written.
bool cvUf2BlockAccepted(uint32_t familyId, uint32_t address, uint32_t length, uint32_t blockNo,
                        uint32_t numBlocks);

// A copy that sends no block for this long has stopped.
#define CV_WRITE_IDLE_MS 5000u

// True when a copy has stopped short of its last block: the host session accepted blocks, the
// last block has not arrived, and no block has been written for CV_WRITE_IDLE_MS since
// lastBlockMs.
bool cvUpdateStalled(bool sessionAccepted, bool updateWritten, uint32_t nowMs, uint32_t lastBlockMs);

// What the board knows when an accepted block arrives.
typedef struct {
  // The block count of the transfer in progress, 0 when none is.
  uint32_t sessionBlocks;
  // The block's own numBlocks and blockNo.
  uint32_t blockNumBlocks;
  uint32_t blockNo;
  // The block lands in a sector this transfer already erased.
  bool sectorErased;
  // That flash holds data other than the block's payload.
  bool targetHoldsOtherData;
  // A write has failed or a block has been refused since the transfer began.
  bool sessionFailed;
} CvUf2SessionInputs;

// Whether an accepted block starts a new transfer instead of joining the one in progress. A
// transfer in progress ends when the block comes from a file with another block count, when it
// lands in a sector this transfer already erased on flash that holds other data, which only a
// second file can do, or when a failed transfer sees a file's first block again. The restart
// erases every sector afresh, so the file dropped again after a failed write gets a clean try.
bool cvUf2SessionRestart(const CvUf2SessionInputs* in);

// Scans one root directory sector of 512 bytes for a file that cannot be firmware: a regular file
// whose extension is one a person might drag by mistake (a raw image, a hex or elf file, an
// archive). Finder's own files never carry these extensions. On a find, writes its 8.3 name as
// NAME.EXT to name[13] and returns true.
bool cvDirNamesWrongFile(const uint8_t* sector, char name[13]);

// The screen update mode shows. A host session's events come first: a failed write or a block
// refused after accepted ones stops the update, accepted blocks show the writing screen, refused
// blocks or a wrong file the wrong-file screen. Then the clock failure, the pulled cable when the
// timeout cannot run, and the waiting screen for the reason.
CvScreen cvUpdateScreen(const CvScreenInputs* in);

void cvDebounceReset(CvDebounce* debounce, bool level);

// Feeds one sample at nowMs. The level follows a sample that has held for holdMs. True when the
// level changed.
bool cvDebounceUpdate(CvDebounce* debounce, bool sample, uint32_t nowMs, uint32_t holdMs);

// ---- Update session -----------------------------------------------------------------------

// The cable's level must hold this long before update mode acts on it.
#define CV_VBUS_DEBOUNCE_MS 50u
// A restart on the host's eject waits this long, so the status of the eject command reaches the
// host first.
#define CV_EJECT_SETTLE_MS  20u

// Update mode's state from its start to its restart. board_hooks.c feeds it the cable, the host,
// the UF2 blocks and the clock, and acts on what each call returns.
typedef struct {
  // The decision's reason, an enum CvBootReason.
  uint32_t reason;
  // The cable, debounced, and whether update mode has acted on its first reading.
  CvDebounce vbus;
  bool cableTracked;
  // The cable came out after a host had connected, and is still out.
  bool cableOut;
  // A host has enumerated since the cable went in; the timeout waits for a pull.
  bool hostSeen;
  bool mounted;
  // When the timeout started counting: update mode's start, or the last pull with nothing written.
  uint32_t timeoutFromMs;
  // Update mode has accepted a UF2 block, so the installed image is partly overwritten.
  bool writeBegun;
  // The last block of an update is written, its image checked good and the boot block marked.
  bool updateWritten;
  // The host ejected the drive after a write began, and when. The box restarts CV_EJECT_SETTLE_MS
  // later.
  bool ejectPending;
  uint32_t ejectedMs;
  // Since the host last mounted the drive, as CvScreenInputs names them, and a copy that stopped
  // short of its last block (cvUpdateStalled).
  bool sessionAccepted;
  bool sessionRefused;
  bool sessionFailed;
  bool sessionStalled;
  // When the last block was written.
  uint32_t lastBlockMs;
  // Application sectors the transfer in progress has written, one bit each from sector 1.
  uint32_t sessionSectors;
  // DIAG.TXT lags what update mode knows.
  bool diagDirty;
} CvUpdateSession;

// A UF2 block as the board's filter sees it.
typedef struct {
  uint32_t familyId;
  uint32_t targetAddr;
  uint32_t payloadSize;
  uint32_t blockNo;
  uint32_t numBlocks;
  // The block count of the transfer in progress, 0 when none is.
  uint32_t sessionBlocks;
} CvUf2BlockInfo;

typedef enum {
  CvBlockRefused,
  CvBlockJoined,
  // Accepted, and the transfer starts over: the board forgets the old one's counts and erases.
  CvBlockRestarted
} CvBlockVerdict;

typedef enum {
  CvCableNone,
  // The cable is in: hold the D+ pull-up.
  CvCableConnect,
  // The cable is out at update mode's start: drop the pull-up.
  CvCableDisconnect,
  // The cable came out: drop the pull-up and post the unplug event.
  CvCableUnplugged,
  // The cable came out after a complete write: restart into the new image.
  CvCableRestart
} CvCableAction;

typedef enum {
  CvTickNone,
  // The eject has settled after a write began: restart. A marked block runs the new image, an
  // unmarked one finds the image invalid and offers the drive again.
  CvTickRestart,
  // The timeout ran out: reset, and the decision runs the application.
  CvTickTimeout
} CvTickAction;

// Update mode's start, with the cable's first raw reading.
void cvSessionStart(CvUpdateSession* s, uint32_t reason, bool cableIn, uint32_t nowMs);

// The cable's raw reading on each pass.
CvCableAction cvSessionCable(CvUpdateSession* s, bool cableIn, uint32_t nowMs);

// The host's mount state on each pass. A mount or an unmount starts a new host session. True on a
// mount, when the board rebuilds the drive's texts.
bool cvSessionMount(CvUpdateSession* s, bool mounted);

// True when the transfer in progress has erased the sector holding `address`. Only then does the
// board read the flash at the block's target for cvSessionBlock.
bool cvSessionSectorErased(const CvUpdateSession* s, uint32_t address, uint32_t sessionBlocks);

// A UF2 block before it is written. targetHoldsOtherData: the sector is erased by this transfer
// and the target holds data that is neither blank nor the block's payload. A refused block is
// never written. A second file in one host session, or the file dropped again after a failed
// transfer, restarts the transfer (cvUf2SessionRestart).
CvBlockVerdict cvSessionBlock(CvUpdateSession* s, const CvUf2BlockInfo* block, bool targetHoldsOtherData);

// After the flash write of an accepted block. A failed one stops the update.
void cvSessionBlockWritten(CvUpdateSession* s, bool written, uint32_t nowMs);

// A root directory sector named a file that cannot be firmware; newName when it differs from the
// last one. Shows the wrong-file screen unless blocks went in.
void cvSessionWrongFile(CvUpdateSession* s, bool newName);

// After the last block of a transfer, with the image check of the flash. True when the board marks
// the boot block for the new image. An image that fails the check stops the update instead.
bool cvSessionLastBlock(CvUpdateSession* s, bool imageGood);

// The host ejected the drive. Once a write has begun the box restarts after CV_EJECT_SETTLE_MS;
// before one, an eject changes nothing.
void cvSessionEjected(CvUpdateSession* s, uint32_t nowMs);

// True when the timeout may run (cvBootTimeoutAllowed).
bool cvSessionTimeoutArmed(const CvUpdateSession* s, const struct CvBootBlock* block, CvImageStatus image);

// Each pass, after the cable and the host: the eject's restart, a stalled copy, the timeout.
CvTickAction cvSessionTick(CvUpdateSession* s, bool timeoutArmed, uint32_t nowMs);

// The screen now.
CvScreen cvSessionScreen(const CvUpdateSession* s, bool imageValid, bool timeoutArmed);

// True when update mode may read every word of the application region at its start: the decision
// allows reading, and no erase of a faulting sector failed.
bool cvRegionReadAllowed(const CvBootDecision* decision, bool eraseFailed);

#ifdef __cplusplus
}
#endif
