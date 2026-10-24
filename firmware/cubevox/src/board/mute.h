// Output mute pins. The sequencing lives in core/mute_sequence; this file reads MUTE_SW and
// drives MUTE_N and XSMT.

#pragma once

#include <stdint.h>

#include "core/settings.h"

// Drives MUTE_N and XSMT low. Call first in setup(), before anything that takes time.
void muteInit();

// Tells the mute logic when the SAI clocks started. The box stays muted until
// kBootUnmuteDelayMs after this, even if MUTE_SW is released.
void muteClocksRunning(uint32_t nowMs);

// Call every loop pass. Reads MUTE_SW, steps the sequence and drives the pins. With TOP SWITCH
// MUTE the switch runs the mute sequence. With BYPASS the sequence ignores the switch and
// muteBypassOn() follows it.
void mutePoll(uint32_t nowMs, TopSwitch role);

// True while TOP SWITCH is BYPASS and MUTE_SW is in its muted position, debounced. The audio
// interrupt reads it once per block.
bool muteBypassOn();

// Mutes through the normal sequence and ignores MUTE_SW until reset. For faults the
// firmware can see coming (a stalled audio interrupt).
void muteLatch();

// True while the output is muted or a mute / release sequence is in progress.
bool muteActive();

// True once MUTE_N has gone low; see MuteSequence::fullyMuted.
bool muteFullyMuted();

// Another reason to stay muted, like the switch. Releasing it runs the normal release sequence.
void muteHold(bool on);

// Blocking mute for DFU entry or a reset: XSMT low, wait, MUTE_N low, wait for the relay.
void muteForReset();
