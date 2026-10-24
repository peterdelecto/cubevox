// SH1106 128x64 OLED on I2C2 through board/sh1106.c, the driver the bootloader shares. Implements the
// Display interface.

#pragma once

#include <stdint.h>

#include "board/sh1106.h"
#include "core/display.h"

constexpr uint8_t kOledAddress = CV_SH1106_ADDRESS;  // 7-bit

// True while the panel answers: its last start and its last write went through. Starts the bus and
// the panel on the first call.
bool sh1106Present();

// Canvas display on the SH1106. Starts the bus and the panel on the first call. A panel that is
// missing or stops answering is restarted on the retry window of core/display.h's PanelLink.
Display& sh1106Display();
