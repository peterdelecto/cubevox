// Owns the bank 2 flash and the record store, and turns them into calls the sketch and the bench
// make: load at boot, request a save, poll, report.

#pragma once

#include <Arduino.h>

#include "core/layout.h"
#include "core/settings.h"

// Boot-time load. Recovers from a bank 2 double ECC fault first (blocking erase, so call it
// before faultWatchdogStart()), then loads the newest valid record into `layout` and `settings`.
// Leaves both untouched when there is no usable record.
void recordBoot(Layout& layout, Settings& settings);

// A settings or layout change. The store writes 2 s after the last call.
void recordRequestSave(const Layout& layout, const Settings& settings, uint32_t nowMs);

// Call every loop pass. Starts at most one flash operation.
void recordPoll(uint32_t nowMs);

// Bench: write the current state now, erase both sectors, print status.
void recordSaveNow();
void recordEraseAll();
void recordPrintStatus(Print& out);

// The "[boot] record: ..." line and any recovery warning.
void recordPrintBoot(Print& out);
