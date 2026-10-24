// The block of DTCM the application shares with the bootloader (board/boot_block.h), as a
// variable. Every field is random after a power-on and only the bootloader makes it valid, so
// readers check before they trust a field.

#pragma once

#include "board/boot_block.h"

CvBootBlock& bootBlock();

// True when a bootloader wrote the block since power-on. Every block version from 1 up counts,
// because a later bootloader only adds fields. Without it the reason, the fault address and the
// summary hold nothing to read.
inline bool bootBlockWritten() { return cvBootBlockInitialised(&bootBlock()); }
