// Arduino compiles only the sketch tree, so the copied platform sources (firmware/platform,
// on the include path) come in by inclusion. h7_dfu_reboot.h defines initVariant(), so it
// must be included in exactly one translation unit.

#include "h7_block_mem.cpp"
#include "h7_dfu_reboot.h"
#include "board/platform.h"

void platformEnterDfu() { dfu_reboot_request(); }
