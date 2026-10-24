// SH1106 OLED of the cubevox panel, write only. See oled.c.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "screens.h"

#ifdef __cplusplus
extern "C" {
#endif

// Frees the bus, starts I2C2 and sends the panel's init sequence. False when the panel does not
// answer; every later call then returns at once.
bool cvOledStart(void);

// Writes a whole frame in the screens.h layout and turns the panel on after the first frame.
void cvOledShow(const uint8_t frame[CV_FRAME_BYTES]);

#ifdef __cplusplus
}
#endif
