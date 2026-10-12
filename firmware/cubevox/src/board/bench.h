// Single-letter bench commands from a serial terminal. Roadmap step 3 replaces these with the JSON
// protocol; until then they drive the first-article fault checks and the cost measurements.

#pragma once

#include "core/compose.h"

// Commands (one letter each):
//   1-8 toggle slot off, 0 all slots on, k pin knobs, c Chasm reverb, r boot report, p stats,
//   f bus fault, w watchdog spin, a stop audio, m reset peaks, v vocal clip, d DFU,
//   s save the layout record now, q record status, e erase both bank 2 sectors.
// Reads pending serial input and runs each command. `bootReport` prints the boot report ('r').
// Returns true when the overrides changed, so the caller republishes the snapshot.
bool benchPoll(void (*bootReport)());

const BenchOverrides& benchOverrides();

// Prints the audio counters, total and per slot.
void benchPrintStats();
