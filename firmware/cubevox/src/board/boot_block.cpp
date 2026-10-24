#include "board/boot_block_access.h"

// firmware/cubevox.ld places the section at CV_BOOT_BLOCK_ADDR. It is NOLOAD, so startup neither
// loads nor zeroes it, and a system reset keeps what the bootloader or the last run wrote.
__attribute__((section(".cv_boot_block"), used)) CvBootBlock gBootBlock;

CvBootBlock& bootBlock() { return gBootBlock; }
