// Panel input: the 16-pot mux scan, per-pot smoothing, stepped knobs and the slot toggles.

#pragma once

#include <stdint.h>

#include "core/knob_filter.h"
#include "core/layout.h"
#include "core/panel.h"

// Mux timing (hardware spec items 10 and 19). The 100 R / 1 nF at the ADC pin and the mux
// source impedance need >= 0.5 ms after a select change. The ADC sample time is a core
// compile-time default, set to 810.5 cycles (>= 27 us) by build.sh.
constexpr uint32_t kMuxSettleUs = 500;
// U107 holds 3V3A off for up to 1030 ms after power-on; a high select before that pushes
// ~20 mA through U101's input clamp into the dead rail. Selects stay low until this passes.
constexpr uint32_t kMuxSelectHoldMs = 1100;
constexpr uint32_t kToggleDebounceMs = 20;

void controlsInit();

// Call every loop pass. Advances the mux scan without blocking and debounces the toggles.
// Step counts come from `layout`; a knob whose count changes re-seeds its step position.
void controlsPoll(uint32_t nowMs, const Layout& layout);

// True once since the last call if any knob, step or toggle changed (and once after the first full scan).
bool controlsTakeChanged();

// Knob values (0..1, or the step index for stepped knobs), toggles and the last moved knob.
PanelReading controlsReading();
