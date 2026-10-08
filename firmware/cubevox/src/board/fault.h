// Fault story (foundation spec 2026-10-08): independent watchdog, Cortex-M fault handlers that
// mute and record before resetting, reset-cause decode, and a watch on the audio interrupt.

#pragma once

#include <Arduino.h>

// Watchdog period. loop() kicks it; the audio interrupt never does.
constexpr uint32_t kWatchdogMs = 2000;
// Longest the audio block counter may sit still while audio is running.
constexpr uint32_t kAudioStallMs = 100;

// Reads and clears the reset cause and any fault record left by the previous run, and routes
// BusFault, UsageFault and MemManage to their own handlers. Call right after muteInit().
void faultInit();

// Starts the watchdog. Call after audioInit(); from then on loop() must call faultWatchdogKick()
// at least every kWatchdogMs.
void faultWatchdogStart();
void faultWatchdogKick();

// Call every loop pass. A stalled audio interrupt latches the mute and reports once.
void faultAudioWatch(uint32_t nowMs, bool audioRunning);

// Boot report lines: reset cause, last fault (or "[boot] fault: none"), audio stall if any.
void faultReport(Print& out);
