// Owns the bank 2 flash and the record store, and turns them into calls the sketch and the bench
// make: load at boot, request a save, poll, report.

#pragma once

#include <Arduino.h>

#include "board/build_kind.h"
#include "core/layout.h"
#include "core/settings.h"

// Boot-time load. Recovers from a bank 2 double ECC fault first (blocking erase, so call it
// before faultWatchdogStart()), then loads the newest valid record into `layout` and `settings`.
// Leaves both untouched when there is no usable record.
void recordBoot(Layout& layout, Settings& settings);

// A settings or layout change. The store writes 2 s after the last call.
void recordRequestSave(const Layout& layout, const Settings& settings, uint32_t nowMs);

// The layout name the next save writes, and the one in force (loaded or set). Empty when never named.
void recordSetName(const char* name);
const char* recordName();

// Sequence of the newest record written to flash; it rises when a save finishes.
uint32_t recordSequence();

// Flash erase or program failures since boot.
uint32_t recordErrors();

// Call every loop pass. Starts at most one flash operation.
void recordPoll(uint32_t nowMs);

// Prints the newest record's sequence, sector, errors, name and settings.
void recordPrintStatus(Print& out);

#if CUBEVOX_BENCH
// Bench drills: write the current state now, erase both sectors.
void recordSaveNow();
bool recordEraseAll();  // false while the store is busy
#endif

// The "[boot] record: ..." line and any recovery warning.
void recordPrintBoot(Print& out);
