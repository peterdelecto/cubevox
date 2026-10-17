// The one way into the ROM DFU bootloader.

#pragma once

// Writes a pending layout or settings save, waits (bounded) for the store to finish, lets the serial
// queue drain, mutes the output and reboots into DFU. Never returns.
[[noreturn]] void enterDfu();
