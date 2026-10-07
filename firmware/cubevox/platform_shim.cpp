// Arduino compiles only the sketch folder, so the copied platform sources come in by inclusion.
// h7_dfu_reboot.h defines initVariant(), so it must be included in exactly one translation unit.

#include "../platform/h7_block_mem.cpp"
#include "../platform/h7_dfu_reboot.h"
#include "platform.h"

void platformEnterDfu() { dfu_reboot_request(); }
