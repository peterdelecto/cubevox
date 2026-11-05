// MENU encoder: quadrature ISR, click debounce, click and hold detection, pin setup. Hands the ui a
// UiInput per pass.

#pragma once

#include <stdint.h>

struct UiInput;  // ui/menu.h

// Sets the pins and attaches the encoder interrupts.
void inputInit();

// Detents turned since the last call, whether the button was released after a short press, and
// whether a press has just reached kMenuHoldMs.
UiInput inputPoll(uint32_t nowMs);
