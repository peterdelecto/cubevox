// Warm start or cold start (stage-survival spec F4). A warm start follows a reset the box caused
// while its power stayed up: a software reset, the independent watchdog or the window watchdog. Every
// other start is cold and keeps the full power-on path. Pure: board/fault supplies the reset flags
// and board/power_sense the rail.

#pragma once

#include <stdint.h>

// RCC->RSR bits. The bootloader stores them in the boot block (CvBootBlock::resetFlags) and clears
// the register. A host test holds these masks equal to the bootloader's names for them.
constexpr uint32_t kResetFlagBor = 1u << 21;
constexpr uint32_t kResetFlagPor = 1u << 23;
constexpr uint32_t kResetFlagSoftware = 1u << 24;
constexpr uint32_t kResetFlagWatchdog = 1u << 26;
constexpr uint32_t kResetFlagWindowWatchdog = 1u << 28;

constexpr uint32_t kResetFlagsPowerLoss = kResetFlagPor | kResetFlagBor;
constexpr uint32_t kResetFlagsBoxCaused = kResetFlagSoftware | kResetFlagWatchdog | kResetFlagWindowWatchdog;

// True when the reset flags hold a box-caused reset and no power-loss flag. PINRSTF decides nothing,
// since this chip sets it on every reset. No flags at all (a boot block the bootloader did not fill)
// is not box-caused.
constexpr bool isBoxCausedReset(uint32_t resetFlags) {
  return (resetFlags & kResetFlagsBoxCaused) != 0u && (resetFlags & kResetFlagsPowerLoss) == 0u;
}

// True when this start is warm: a box-caused reset, and the power sense has a reading with no trip.
constexpr bool isWarmStart(uint32_t resetFlags, bool powerHasReading, bool powerTripped) {
  return isBoxCausedReset(resetFlags) && powerHasReading && !powerTripped;
}
