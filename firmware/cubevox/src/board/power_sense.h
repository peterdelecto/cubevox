// Power-fail mute (hardware spec F2 and F3). ADC1 converts VA_SENSE without pause, and its analog
// watchdog interrupt drops XSMT and MUTE_N by register the moment 5VA falls under the trip level,
// whatever loop() is doing. The release waits in loop() for the rail to hold (core/power_fail).
// ADC1 and ADC2 share a reset and a clock, so nothing else may use either of them.

#pragma once

#include <Arduino.h>

// Brings up ADC1 on VA_SENSE and arms the watchdog interrupt. Call right after muteInit(). Returns
// false on failure; powerSenseReport() says why, and the box runs without the power-fail mute.
bool powerSenseInit();

// Call every loop pass, before mutePoll(). Releases the mute once the rail has held for
// kPowerHoldMs, and prints one line per trip and per release.
void powerSensePoll(uint32_t nowMs);

// Power-fail trips since boot.
uint32_t powerSenseTrips();

// The "[boot] power sense ..." line, or "[ERROR] power sense: ..." when powerSenseInit failed.
void powerSenseReport(Print& out);
