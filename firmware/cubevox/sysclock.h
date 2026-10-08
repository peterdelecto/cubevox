// System clock tree (spec 2026-10-08 step 0). SystemClock_Config() is overridden in sysclock.cpp:
// HSE 25 MHz drives PLL1 (core) and PLL3 (SAI, see audio.cpp); USB runs from PLL1Q, HSI48 stays off.

#pragma once

#include <Arduino.h>

// Silicon revision read from DBGMCU; 480 MHz needs rev V.
bool clockIsRevV();

// Prints revision, ROM bootloader ID, PLL source and the clocks that matter to a terminal.
void clockReport(Print& out);
