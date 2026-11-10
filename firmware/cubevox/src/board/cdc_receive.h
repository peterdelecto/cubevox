// The CDC receive callback the USB interrupt calls for each packet the host sends. A full receive
// queue holds the host off until the main loop reads.

#pragma once

// Puts the firmware's receive callback in place of the core's. Call before Serial.begin().
void cdcReceiveInstall();
