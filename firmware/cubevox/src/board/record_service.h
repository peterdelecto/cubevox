// Owns the record store on the bank 2 flash (shared with the diagnostic log, board/diag_service),
// and turns it into calls the sketch and the bench make: load at boot, request a save, poll, report.

#pragma once

#include <Arduino.h>

#include "board/build_kind.h"
#include "core/layout.h"
#include "core/settings.h"

// Boot-time load. Recovers from a bank 2 double ECC fault first, in a record sector or in the
// diagnostic log's sector (blocking erase, so call it after faultInit() and before
// faultWatchdogStart()), then loads the newest valid record into
// `layout` and `settings`. A record whose layout fails validation gives the default layout and a
// save that rewrites it. Leaves both untouched when there is no usable record.
void recordBoot(Layout& layout, Settings& settings, uint32_t nowMs);

// A settings or layout change. The store writes 2 s after the last call.
void recordRequestSave(const Layout& layout, const Settings& settings, uint32_t nowMs);

// The layout name the next save writes, and the one in force (loaded or set). Empty when never named.
void recordSetName(const char* name);
const char* recordName();

// Sequence of the newest record written to flash; it rises when a save finishes.
uint32_t recordSequence();

// Flash erase or program failures since boot.
uint32_t recordErrors();

// Call every loop pass. Starts at most one flash operation and reports a failed save once.
void recordPoll(uint32_t nowMs);

// Writes a pending save now and polls until the store is idle, kicking the watchdog. Returns false
// when it is still busy after `timeoutMs`. For the moments before a reset.
bool recordFlush(uint32_t timeoutMs);

// Prints the newest record's sequence, sector, errors, name and settings.
void recordReportStatus(Print& out);

#if CUBEVOX_BENCH
// Bench drills: write the current state now, erase both sectors.
void recordSaveNow();
bool recordEraseAll();  // false while the store is busy
#endif

// The "[boot] record: ..." line and any recovery or scan warning.
void recordReportBoot(Print& out);
