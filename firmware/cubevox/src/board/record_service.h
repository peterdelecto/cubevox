// Owns the record store on the bank 2 flash (shared with the diagnostic log, board/diag_service),
// and turns it into calls the sketch and the bench make: load at boot, request a save, poll, report.

#pragma once

#include <Arduino.h>

#include "board/build_kind.h"
#include "core/layout.h"
#include "core/settings.h"

// Boot-time load. First recovers from a bank 2 double ECC fault, whether the last run hit it or the
// bootloader reported its address, in a record sector or in the diagnostic log's sector. The
// recovery waits on flash erases, so call it after faultInit() and before faultWatchdogStart().
// Then loads the newest valid record into `layout` and `settings`. A record whose layout fails
// validation gives the default layout and a save that rewrites it. Leaves both untouched when
// there is no usable record.
void recordBoot(Layout& layout, Settings& settings, uint32_t nowMs);

// A settings or layout change. The store writes kSaveDebounceMs after the last call, and writes
// nothing when flash already holds the same state.
void recordRequestSave(const Layout& layout, const Settings& settings, uint32_t nowMs);

// A pending save starts on the next poll instead of at the end of the debounce.
void recordSkipDebounce();

// True while a save is pending or being written.
bool recordBusy();

// False when the boot's ECC recovery could not erase the diagnostic log's sector. Reading that
// sector would fault again, so the log stays closed.
bool recordLogSectorReadable();

// The layout name the next save writes, and the one in force (loaded or set). Empty when never named.
void recordSetName(const char* name);
const char* recordName();

// Sequence of the newest record written to flash; it rises when a save finishes.
uint32_t recordSequence();

// Flash erase or program failures since boot.
uint32_t recordErrors();

// The flash address of the bootloader's last read fault (0 for none, or when the bootloader did not
// write the boot block), as recordBoot() found it. recordBoot() clears a bank 2 address from the
// block once it has erased that sector, so this keeps the value for the diagnostic log.
uint32_t recordBootFaultAddress();

// Call every loop pass. Starts at most one flash operation and reports a failed save once.
void recordPoll(uint32_t nowMs);

// Prints the newest record's sequence, sector, errors, name and settings.
void recordReportStatus(Print& out);

#if CUBEVOX_BENCH
// Bench drills: write the current state now, even when unchanged; erase both sectors.
void recordSaveNow();
bool recordEraseAll();  // false while the store is busy
#endif

// The "[boot] record: ..." line and any recovery or scan warning.
void recordReportBoot(Print& out);
