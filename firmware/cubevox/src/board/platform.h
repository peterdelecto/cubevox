// Entry points into the copied platform code (../platform), which is compiled in platform_shim.cpp.

#pragma once

// Reboots into the STM32 ROM DFU bootloader over the rear USB-C. Does not return.
void platformEnterDfu();
