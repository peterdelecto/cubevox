// Warm or cold start, and how long a warm start took to reach its first block, its first snapshot
// and the relay (stage-survival spec F4). All times count from the first line of setup().

#pragma once

#include <Arduino.h>
#include <stdint.h>

// Marks the start of setup(). Call first.
void startTimingBegin(uint32_t nowMs);

// Records the warm or cold decision (core/restart_rule).
void startTimingSetWarm(bool warm);

// True when this start is warm.
bool startIsWarm();

// The block interrupt finished a block. Cheap after the first call, so it runs every block.
void startTimingNoteBlock();

// loop() published the first snapshot to the audio interrupt.
void startTimingNoteSnapshot(uint32_t nowMs);

// MUTE_N went high for the first time since the start.
void startTimingNoteRelay(uint32_t nowMs);

// The "[boot] start ..." line. A warm start prints the three times in ms, or "not yet" for one that
// has not happened.
void startTimingReport(Print& out);
