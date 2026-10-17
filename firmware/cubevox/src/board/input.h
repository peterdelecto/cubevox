// MENU encoder: quadrature ISR, click debounce, pin setup. Hands the ui a UiInput per pass.

#pragma once

#include <stdint.h>

struct UiInput;  // ui/menu.h

// Sets the pins and attaches the encoder interrupts.
void inputInit();

// Detents turned since the last call and whether the button was pressed.
UiInput inputPoll(uint32_t nowMs);
