// The block of DTCM the application shares with the bootloader (board/boot_block.h), as a
// variable. Every field is random after a power-on and only the bootloader makes it valid, so
// readers check before they trust a field.

#pragma once

#include "board/boot_block.h"

CvBootBlock& bootBlock();

// True when the bootloader wrote the block since power-on. Any other version word is power-on
// garbage, and the reason, the fault address and the summary hold nothing to read.
inline bool bootBlockWritten() { return bootBlock().blockVersion == CV_BOOT_BLOCK_VERSION; }
