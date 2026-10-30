// Output mute pins. The sequencing lives in core/mute_sequence; this file reads MUTE_SW and
// drives MUTE_N and XSMT.

#pragma once

#include <stdint.h>

#include "core/settings.h"

// Drives MUTE_N and XSMT low. Call first in setup(), before anything that takes time.
void muteInit();

// Tells the mute logic the SAI clocks run. The box stays muted until then, even if MUTE_SW is
// released. XSMT rises as soon as it can, into the open relay. The first release after power-on
// closes the relay no earlier than kRelayCloseAfterPowerOnMs after muteInit().
void muteClocksRunning();

// True while the audio output must sit at digital zero: from the fully muted state until the relay
// has closed on a release (core/mute_sequence). The audio interrupt reads it once per block and
// holds the layout fade at zero, then fades up when it clears.
bool muteOutputHeld();

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

// Reasons besides the switch to stay muted. Each owner sets and clears its own.
enum class MuteReason : uint8_t {
  Panel = 1u << 0,      // the sketch: the jack relay, unseeded knobs, the bench
  PowerFail = 1u << 1,  // 5VA under its trip level (board/power_sense)
  Audio = 1u << 2,      // an audio stream fault until its restart (board/audio)
};

// The Panel reason. Releasing the last reason runs the normal release sequence.
void muteHold(bool on);

// Holds or releases `reason` through the normal sequence. Call from loop().
void muteHoldFor(MuteReason reason, bool on);

// Drives XSMT and MUTE_N low by register at once and holds the mute for `reason` until
// muteTripClear(). For faults that cannot wait for loop(): safe in any interrupt.
void muteTrip(MuteReason reason);

// Ends a trip from loop(). The output releases through the normal sequence once no reason is left.
void muteTripClear(MuteReason reason);

// Blocking mute for DFU entry or a reset: XSMT low, wait, MUTE_N low, wait for the relay.
void muteForReset();
