// Fault story (foundation spec 2026-10-08): independent watchdog, Cortex-M fault handlers that
// mute and record before resetting, reset-cause decode, a watch on the audio interrupt, and a
// measure of the main stack.

#pragma once

#include <Arduino.h>

#include "board/audio.h"
#include "board/boot_block.h"
#include "board/build_kind.h"
#include "core/audio_fault_cause.h"

// Takes the reset cause from the boot block and loads any fault record left by the previous run,
// enables the MemManage, BusFault and UsageFault exceptions, and paints the unused main stack. Call
// early in setup(), right after the mute pins are set up.
void faultInit();

// True once faultInit() has run. The ECC recovery reads the fault record faultInit() loads.
bool faultInitDone();

struct RecordReadLimits;

// The record store's read limits, kept in .cv_noinit beside the fault record so they outlive a
// reset. A power-on leaves them random; RecordStore::useReadLimits() checks their seal.
RecordReadLimits& faultRecordReadLimits();

// The reset flags (RCC->RSR bits) of this boot, as the bootloader stored them in the boot block
// (CvBootBlock::resetFlags, block version 2 and up). The bootloader reads and clears the register
// and owns it, so the application never touches it. Valid after faultInit().
uint32_t faultResetFlags();

// True when the first reset flag set, in the order of the boot report, is power-on. A power-up
// also raises the brown-out and pin flags, which sort behind it. Valid after faultInit().
bool faultResetWasPowerOn();

// True when the previous run left a fault record. Valid after faultInit().
bool faultRecordPresent();

// The audio fault that reset the box at the end of the previous run, so the splash names it, or
// None. faultInit() takes the sealed cause from .cv_noinit and clears it, so one fault gives one
// boot with the error; a power-on or any other reset reads None. Valid after faultInit().
AudioFaultCause faultLastAudioCause();

// Clears the sealed cause, for a planned reset that ends in update mode instead.
void faultForgetAudioCause();

// True when the previous run died on a double ECC error in flash bank 2 (FLASH->SR2 DBECCERR);
// `address` is the failing flash word. Valid after faultInit().
bool faultLastWasBank2Ecc(uint32_t& address);

// Sets the watchdog's own 2 s period over the 16 s the bootloader started it with. Call at the end
// of setup(); from then on loop() must call faultWatchdogKick() at least every 2 s.
void faultWatchdogStart();
void faultWatchdogKick();

// Call every loop pass with what audioPoll returned. New SAI or DMA transfer errors are reported at
// most once a second. A stream restart queues an audio entry in the diagnostic log. A fault past
// the restart budget, a failed restart, or a stream that stops advancing for kAudioStallMs while it
// runs mutes at once, queues an audio entry naming the cause by its code (A1 to A3), seals the cause
// for the next splash, and starts a planned reset that brings the sound back on the power-on path.
void faultAudioWatch(uint32_t nowMs, bool audioRunning, AudioEvent event);

// True once the stall watch has muted the output for a stall.
bool faultAudioStalled();

// Bytes of the main stack no code has touched since faultInit() painted it.
uint32_t faultStackUntouchedBytes();

// The clock setup step that failed. The values are the contract's, so the bootloader names the
// same step from the record.
enum class ClockStep : uint32_t {
  Supply = CvClockStepSupply,
  VoltageScale = CvClockStepVoltageScale,
  Oscillator = CvClockStepOscillator,
  ClockTree = CvClockStepClockTree,
  PeripheralClocks = CvClockStepPeripheralClocks
};

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
// CRC verdict, earlier failed starts, last update reason), the watchdog period, the untouched stack,
// audio stall if any.
void faultReport(Print& out);

// The previous run's fault record for a diagnostic log entry: the kind, pc, lr, cfsr and bfar, as
// in "bus pc=0x8021A4C lr=0x8020F31 cfsr=0x8200 bfar=0xDEADBEE0", or the clock step that failed.
// Empty without a record. NUL-terminated within `size`.
void faultSummaryText(char* out, size_t size);

// Short text for the boot block's appSummary. It holds the first reset cause that was set at boot
// and the kind of the last fault, as in "power-on none" or "watchdog bus", NUL-terminated within
// `size`.
void faultResetText(char* out, size_t size);
