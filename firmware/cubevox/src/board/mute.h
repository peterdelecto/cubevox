// Output mute: MUTE_SW (low = muted) drives the DAC soft mute (XSMT) and the output relay (MUTE_N).
// Mute order is XSMT low, wait, MUTE_N low. Release order is MUTE_N high, wait, XSMT high.
// The chain keeps running while muted.

#pragma once

#include <stdint.h>

// Timing in ms. The XSMT lead is spec note F3 (>= 3.4 ms at 48 kHz) with margin.
// The relay settle covers the G6K-2F-Y operate time. The boot delay is spec note F2.
constexpr uint32_t kXsmtLeadMs = 5;
constexpr uint32_t kRelaySettleMs = 20;
constexpr uint32_t kBootUnmuteDelayMs = 500;

// Drives MUTE_N and XSMT low. Call first in setup(), before anything that takes time.
void muteInit();

// Tells the mute logic when the SAI clocks started. The box stays muted until
// kBootUnmuteDelayMs after this, even if MUTE_SW is released.
void muteClocksRunning(uint32_t nowMs);

// Call every loop pass. Reads and debounces MUTE_SW and steps the sequence.
void mutePoll(uint32_t nowMs);

// Mutes through the normal sequence and ignores MUTE_SW until reset. For faults the
// firmware can see coming (a stalled audio interrupt).
void muteLatch();

// True while the output is muted or a mute / release sequence is in progress.
bool muteActive();
