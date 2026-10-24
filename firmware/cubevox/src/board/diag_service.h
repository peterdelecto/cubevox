// The diagnostic log (spec 2026-10-24, Diagnostics). Bank 2 sector 2 holds 256-byte entries that
// the application appends and the bootloader lists in DIAG.TXT. A start writes one entry when the
// reset was not a plain power-on or left a fault record, and a request for update mode writes one
// before the reset. Entry layout and sector address live in board/boot_block.h.

#pragma once

#include <Arduino.h>

// Opens the log and queues the boot entry when this start calls for one. Call after recordBoot()
// (its ECC recovery makes an unreadable sector readable before the scan) and after
// bootHealthInit() (the failed-start count and the bootloader's reason). Then lifts the
// bootloader's mark on the log sector, because the scan has read the sector.
void diagInit();

// Call every loop pass. Starts at most one flash operation and reports a failed write once.
void diagPoll();

// Queues the entry for a request to enter update mode, with the live uptime and counters.
void diagNoteUpdateRequest();

// Writes the queued entry now and polls until it is done, kicking the watchdog. Returns false and
// prints a warning when `timeoutMs` passes first. A sector erase the entry needs does not count
// against the timeout, because a reset would cut it short and leave the sector half erased.
bool diagFlush(uint32_t timeoutMs);

// The "[boot] diag log: ..." line: the entry count and last sequence, empty, or error.
void diagReportBoot(Print& out);
