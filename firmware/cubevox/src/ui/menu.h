// MENU encoder state machine and the OLED screens.
// Drawing goes through the Display interface: the SH1106 driver when the panel answers on
// I2C2, otherwise a serial-logging stub. Lines are 16 columns, the SH1106 at the 8x8 font.

#pragma once

#include <stdint.h>

#include "core/display.h"
#include "core/settings.h"

// Returned when no SH1106 driver is linked: prints each changed screen to Serial.
Display& stubDisplay();

void menuInit(Display& display);

// Call every loop pass. Reads the encoder state, runs the menu, redraws on change.
// `muted` and the last-moved knob feed the idle screen.
void menuPoll(uint32_t nowMs, bool muted);

// Settings the menu edits. The reverb engine is not one of them: the placed reverb card picks it.
const Settings& menuSettings();

// True once after a setting changed.
bool menuTakeSettingsChanged();

// Output trim for the LINE / INSTRUMENT choice, applied after the chain.
float menuOutputGainDb();
