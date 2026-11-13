// The part of the updater that runs from AXI SRAM: the writes to sector 0, the erase of the
// updater's own sector and the reset. updater.ld copies it there at the start, so it keeps running
// while bank 1 is busy and after the updater's flash is gone. Nothing in it calls code in flash;
// build.sh checks the linked code for such a call.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "boot_block.h"

#ifdef __cplusplus
extern "C" {
#endif

// The RAM copy of sector 0 as the new bootloader fills it: its bytes, then 0xFF to the end.
extern uint8_t cvSector0Image[CV_FLASH_SECTOR_SIZE];

// Sector 0 as the updater found it, the old bootloader.
extern uint8_t cvSector0Old[CV_FLASH_SECTOR_SIZE];

// The vector table the updater runs on once the RAM code is in place.
extern const uint32_t cvRamVectors[];

// Starts the millisecond clock from the cycle counter at `cyclesPerMs`.
__attribute__((long_call)) void cvRamClockStart(uint32_t cyclesPerMs);

// Milliseconds since cvRamClockStart. Calls must come less than a counter wrap apart (67 s at
// 64 MHz).
__attribute__((long_call)) uint32_t cvRamMs(void);

// Reloads IWDG1, which the bootloader started at 16 s before the jump.
__attribute__((long_call)) void cvWatchdogKick(void);

// Unlocks FLASH_CR1 when it is locked. True once it is unlocked.
__attribute__((long_call)) bool cvBank1Unlock(void);

// Steps 5 to 8 of the spec: erases sector 0, writes the first `writeBytes` bytes of
// cvSector0Image (whole flash words), reads the sector back and compares it with the image, all
// again until it compares. Then it sets the bootloader-updated mark (boot_decide.h) and copies
// `summary` into the boot block, erases the first sector of
// the application region and resets. A fault from here on comes back here. Three passes in a row
// that leave sector 0 equal to cvSector0Old end the run as cvUpdaterLeave does: the old bootloader
// is whole.
__attribute__((long_call, noreturn)) void cvSector0Install(uint32_t writeBytes, const char* summary);

// Leaves sector 0 as it is: copies `summary` into the boot block, erases the first sector of the
// application region and resets, so the bootloader offers the drive with no application installed.
__attribute__((long_call, noreturn)) void cvUpdaterLeave(const char* summary);

#ifdef __cplusplus
}
#endif
