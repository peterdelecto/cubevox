// The one way into the update drive (spec 2026-10-24): the bootloader stays in update mode when
// the application asks for it and resets.

#pragma once

#include <stdint.h>

// Writes the summary the bootloader prints in DIAG.TXT, queues the diagnostic entry for the request,
// and starts the planned reset into update mode (board/planned_reset). loop() keeps running while a
// pending save and the entry reach flash; then the serial queue drains, the output mutes and the
// box resets. A second call does nothing.
void enterDfu(uint32_t nowMs);
