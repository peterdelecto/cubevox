// Panel input: the 16-pot mux scan, per-pot smoothing, stepped knobs and the slot toggles.

#pragma once

#include <stdint.h>

#include "board/build_kind.h"
#include "core/layout.h"
#include "core/panel.h"

void controlsInit();

// Call every loop pass. Advances the mux scan without blocking and debounces the toggles. Step counts
// come from `layout`; when a card change alters a knob's step count the knob re-seeds before this
// call returns, so the reading that follows matches the layout.
void controlsPoll(uint32_t nowMs, const Layout& layout);

// True once every pot has been read at least once. Until then the panel values are not real.
bool controlsSeeded();

// True once since the last call if any knob, step or toggle changed (and once after the first full scan).
bool controlsTakeChanged();

// Knob values (0..1, or the step index for stepped knobs), toggles and the last moved knob.
PanelReading controlsReading();

// True once per touch, with the knob that moved past the dead band since the last call. Several
// moves in between report the last. The OLED readout follows it.
bool controlsTakeTouch(LastMoved& out);

#if CUBEVOX_BENCH
// Unfiltered pot read, 0..1, for the panel self-test. `muxChannel` is 0-based; valid once controlsSeeded().
float controlsRawPot(int muxChannel);

// Debounced toggle state; true = on (throw to GND).
bool controlsToggleOn(int slot);
#endif
