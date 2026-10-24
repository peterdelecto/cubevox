// The resets the firmware plans: the way into update mode and the reset after an audio stall.
// loop() keeps running until the layout record and the diagnostic log have written what they hold,
// so a reset never cuts a flash write short; then the output mutes in order and the box resets.

#pragma once

#include <stdint.h>

enum class PlannedReset : uint8_t {
  AudioStall,  // a plain reset that brings the sound back
  Update,      // the bootloader stays in update mode
};

// Starts the wait and lets a pending save skip its debounce. An update request replaces a stall
// reset that is already waiting; any other second call changes nothing.
void plannedResetStart(PlannedReset why, uint32_t nowMs);

// True from plannedResetStart() until the reset.
bool plannedResetPending();

// Call every loop pass. Once both stores are idle, or kResetWaitMs have passed with no erase
// running, drains the serial queue, mutes the output, asks for update mode when that is the reason,
// and resets.
void plannedResetPoll(uint32_t nowMs);
