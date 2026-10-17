// The boot report for the first-article checklist, printed whenever a terminal opens the port.

#pragma once

// Prints the firmware version, build kind, reset cause, record, clocks, pins, CPU features, OLED
// and audio lines through serialOut(). `oledFound` is what setup() learned from the panel probe.
void bootReport(bool oledFound);
