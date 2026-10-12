// Single-letter bench commands from a serial terminal. Roadmap step 3 replaces these with the JSON
// protocol; until then they drive the first-article fault checks and the cost measurements.

#pragma once

#include "core/canvas.h"
#include "core/compose.h"
#include "core/settings.h"

// Commands (one letter each):
//   1-8 toggle slot off, 0 all slots on, k pin knobs, c Chasm reverb, r boot report, p stats,
//   f bus fault, w watchdog spin, a stop audio, m reset peaks, v vocal clip, d DFU,
//   s save the layout record now, q record status, e erase both bank 2 sectors.
// Panel self-test (spec 2026-10-12 panel test): t on/off, l summary, g raw dump of all pots and
//   toggles; while on, i steps K103 MIC/PEDAL, x holds/releases the output mute, o OLED pattern.
// Reads pending serial input and runs each command. `bootReport` prints the boot report ('r').
// Returns true when the overrides changed, so the caller republishes the snapshot.
bool benchPoll(void (*bootReport)());

const BenchOverrides& benchOverrides();

// Prints the audio counters, total and per slot.
void benchPrintStats();

// Panel self-test. Call every loop pass after controlsPoll(); prints moved pots and toggle edges.
void benchPanelPoll(uint32_t nowMs);

// The jack the sketch should drive: `requested`, or the K103 state the 'i' command forced.
// The change still goes through the JackSwitch mute dip.
InputJack benchJack(InputJack requested);

// True while the 'x' command holds the output muted.
bool benchMuteHeld();

// While the OLED pattern is up, draws it and returns true; the caller skips its own draw.
bool benchDrawPanelPattern(Canvas& canvas);
