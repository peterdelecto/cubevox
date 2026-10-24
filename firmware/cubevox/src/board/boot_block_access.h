// The block of DTCM the application shares with the bootloader (board/boot_block.h), as a
// variable. Every field is random after a power-on and only the bootloader makes it valid, so
// readers check before they trust a field.

#pragma once

#include "board/boot_block.h"

CvBootBlock& bootBlock();
