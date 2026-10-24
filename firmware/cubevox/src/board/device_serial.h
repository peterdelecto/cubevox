// The MCU's 96-bit unique id as 24 uppercase hex characters, bytes in address order. TinyUF2 serves
// the same text as the USB serial of the update drive, so one id names the box in both programs.

#pragma once

const char* deviceSerial();
