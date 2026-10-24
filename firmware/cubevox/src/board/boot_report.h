// The boot report for the first-article checklist, printed whenever a terminal opens the port.

#pragma once

// Prints through serialOut() the firmware version, build kind and serial; the reset cause, last
// fault, image, watchdog and stack lines; the option bytes and any bootloader fault; then the
// record, diag log, clocks, pins, CPU features, OLED, audio, power sense and pot lines. `oledFound`
// is what setup() learned from the panel probe.
void bootReport(bool oledFound);
