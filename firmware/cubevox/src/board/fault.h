// Fault story (foundation spec 2026-10-08): independent watchdog, Cortex-M fault handlers that
// mute and record before resetting, reset-cause decode, and a watch on the audio interrupt.

#pragma once

#include <Arduino.h>

#include "board/build_kind.h"

// Reads and clears the reset cause and any fault record left by the previous run, and enables the
// MemManage, BusFault and UsageFault exceptions. Call right after muteInit().
void faultInit();

// True once faultInit() has run. The ECC recovery reads the fault record faultInit() loads.
bool faultInitDone();

// The reset flags (RCC->RSR) as this boot read them, before faultInit() cleared them. Valid after
// faultInit().
uint32_t faultResetFlags();

// True when the first reset flag set, in the order of the boot report, is power-on. A power-up
// also raises the brown-out and pin flags, which sort behind it. Valid after faultInit().
bool faultResetWasPowerOn();

// True when the previous run left a fault record. Valid after faultInit().
bool faultRecordPresent();

// True when the previous run died on a double ECC error in flash bank 2 (FLASH->SR2 DBECCERR);
// `address` is the failing flash word. Valid after faultInit().
bool faultLastWasBank2Ecc(uint32_t& address);

// Starts the watchdog. Call after audioInit(); from then on loop() must call faultWatchdogKick()
// at least every watchdog period (2 s).
void faultWatchdogStart();
void faultWatchdogKick();

// Call every loop pass. A stalled audio interrupt latches the mute and reports once; new SAI or DMA
// transfer errors are reported at most once a second.
void faultAudioWatch(uint32_t nowMs, bool audioRunning);

// The clock setup step that failed.
enum class ClockStep : uint32_t { Supply = 1, VoltageScale, Oscillator, ClockTree, PeripheralClocks };

// Records the failed clock step in the fault record and resets. Runs before main(), so it touches
// no peripheral beyond the reset request. The next boot report names the step.
[[noreturn]] void faultClockFailure(ClockStep step);

#if CUBEVOX_BENCH
// Bench drills. The fault handler records a forced bus fault; the spin starves the watchdog.
// Each prints a line and flushes the serial queue (the ok reply included) before it acts.
void faultForceBusFault();
[[noreturn]] void faultSpinForWatchdog();
#endif

// Boot report lines: reset cause, last fault (or "[boot] fault: none"), the image line (version,
// CRC verdict, earlier failed starts, last update reason), audio stall if any.
void faultReport(Print& out);

// Short text for the boot block's appSummary. It holds the first reset cause that was set at boot
// and the kind of the last fault, as in "power-on none" or "watchdog bus", NUL-terminated within
// `size`.
void faultSummaryText(char* out, size_t size);
