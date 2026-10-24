// The diagnostic log (spec 2026-10-24, Diagnostics). Bank 2 sector 2 holds 256-byte entries that
// the application appends and the bootloader lists in DIAG.TXT. A start writes one entry when the
// reset was not a plain power-on, left a fault record or met a failed option byte step. A request
// for update mode writes one before the reset, and so does every audio fault. Entry layout and
// sector address live in board/boot_block.h.

#pragma once

#include <Arduino.h>

// Opens the log and queues the boot entry when this start calls for one. Call after recordBoot()
// (its ECC recovery makes an unreadable sector readable before the scan, and it holds the
// bootloader's fault address) and after bootHealthInit() (the failed-start count and the
// bootloader's reason). The log stays closed when the recovery could not erase its sector.
void diagInit();

// Call every loop pass. Starts at most one flash operation and reports a failed write once.
void diagPoll();

// Queues the entry for a request to enter update mode, with the live uptime and counters.
void diagNoteUpdateRequest();

// Queues the entry for an audio fault: a stream restart, a latched mute or a stall.
void diagNoteAudioFault();

// True while an entry is queued or being written.
bool diagBusy();

// The "[boot] diag log: ..." line: the entry count and last sequence, empty, or error.
void diagReportBoot(Print& out);
