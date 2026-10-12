// Single-letter bench commands from a serial terminal. Roadmap step 3 replaces these with the JSON
// protocol; until then they drive the first-article fault checks and the cost measurements.

#pragma once

#include "core/compose.h"

// Reads pending serial input and runs each command. `bootReport` prints the boot report ('r').
// Returns true when the overrides changed, so the caller republishes the snapshot.
bool benchPoll(void (*bootReport)());

const BenchOverrides& benchOverrides();

// Prints the audio counters, total and per slot.
void benchPrintStats();
