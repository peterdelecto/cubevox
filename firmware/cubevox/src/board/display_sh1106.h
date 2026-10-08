// SH1106 128x64 OLED on I2C2, driven in U8x8 text mode. Implements the menu Display interface.

#pragma once

#include "core/display.h"

// True when the panel acknowledges its address on the bus. Starts the bus on first call.
bool sh1106Present();

// Text display on the SH1106. Starts the bus and panel on first call.
Display& sh1106Display();
