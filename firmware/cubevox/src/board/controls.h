// Panel input: the 16-pot mux scan, per-pot smoothing, stepped knobs and the slot toggles. ADC3
// reads the mux, set up once; the core's analogRead is never called, because each call resets the
// ADC it uses and ADC1 carries the power-fail watch.

#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "board/build_kind.h"
#include "core/layout.h"
#include "core/panel.h"

// Sets up the mux selects, ADC3 and the toggles. A failed ADC leaves the panel unread, so the box
// stays muted; controlsReport() says why.
void controlsInit();

// Call every loop pass. Each pass takes one step of the mux scan (select, start a conversion or
// read it) and never waits on the ADC; it also debounces the toggles. A conversion that fails or
// times out drops that pot's sample for the round. Step counts come from `layout`; when a card
// change alters a knob's step count the knob re-seeds before this call returns, so the reading
// that follows matches the layout.
void controlsPoll(uint32_t nowMs, const Layout& layout);

// The "[boot] pots ..." line with the samples dropped since boot, or "[ERROR] pots: ..." when ADC3
// would not come up.
void controlsReport(Print& out);

// True once every pot has been read at least once. Until then the panel values are not real.
bool controlsSeeded();

// True once since the last call if any knob, step or toggle changed (and once after the first full scan).
bool controlsTakeChanged();

// Knob values (0..1, or the step index for stepped knobs), toggles and the last moved knob.
PanelReading controlsReading();

// True once per touch, with the knob that moved past the dead band since the last call. Several
// moves in between report the last. The OLED readout follows it.
bool controlsTakeTouch(LastMoved& out);

// True once per flip, with the slot whose toggle settled on a new level since the last call and
// that level. Several flips in between report the last. The level read at power-on is the
// baseline and raises no flip. The OLED readout follows it.
bool controlsTakeToggle(ToggleFlip& out);

#if CUBEVOX_BENCH
// Unfiltered pot read, 0..1, for the panel self-test. `muxChannel` is 0-based; valid once controlsSeeded().
float controlsRawPot(int muxChannel);

// Debounced toggle state; true = on (throw to GND).
bool controlsToggleOn(int slot);
#endif
