// Tells the bootloader that a start was healthy (spec 2026-10-24, Boot sequence 6) and keeps what
// it left in the boot block for the boot report and `status`.

#pragma once

#include <stdint.h>

// Reads the boot block and opens the window. Call at the end of setup(), after faultWatchdogStart().
void bootHealthInit();

// Call every loop pass. Once the window has run with the audio interrupt advancing, it zeroes the
// bootloader's start counter, one time.
void bootHealthPoll(uint32_t nowMs);

// Earlier starts that never reported healthy, as the bootloader's counter stood at boot.
uint32_t bootHealthFails();

// Why the bootloader last stayed in update mode, as DIAG.TXT spells it.
const char* bootHealthLastReason();

// The same reason as the CvBootReason number. Zero when the bootloader did not write the block.
uint32_t bootHealthLastReasonCode();
