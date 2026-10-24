// The one way into the update drive (spec 2026-10-24): the bootloader stays in update mode when
// the application asks for it and resets.

#pragma once

// Writes a pending layout or settings save, waits (bounded) for the store to finish, lets the serial
// queue drain, mutes the output, asks the bootloader for update mode and resets. Never returns.
[[noreturn]] void enterDfu();
