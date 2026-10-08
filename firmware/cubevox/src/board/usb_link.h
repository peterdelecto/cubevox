// USB attach gated on VBUS (spec 2026-10-08 step 0, Astra USB review item 7). The box is
// self-powered, so the CDC device starts only while PA9 reads VBUS and stops when it drops.

#pragma once

#include <Arduino.h>

constexpr uint32_t kVbusDebounceMs = 50;

void usbLinkInit();

// Starts or stops the CDC device as VBUS comes and goes.
void usbLinkPoll(uint32_t nowMs);

// True once each time a terminal opens the port (DTR rises).
bool usbLinkTerminalOpened();
