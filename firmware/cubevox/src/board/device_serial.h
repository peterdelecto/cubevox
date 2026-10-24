// The MCU's 96-bit unique id as 24 uppercase hex characters, bytes in address order. TinyUF2 serves
// the same text as the USB serial of the update drive. The application's CDC device keeps the
// core's 12-character serial, so `hello` and the boot report carry this one, and bringup.py matches
// it with INFO_UF2.TXT.

#pragma once

const char* deviceSerial();
