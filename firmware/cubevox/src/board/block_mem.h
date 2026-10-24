// Adapted from Teensy-H7-Port, lib/platform/h7/h7_block_mem.h.
//
// Where audio memory lives on the STM32H743, and why. Two facts drive it.
//
//   1. DMA1 and DMA2 cannot reach DTCM, the fastest RAM on the chip.
//   2. The D-cache lies to a DMA engine. A buffer the CPU touched sits dirty in cache while DMA
//      reads stale memory, and a buffer DMA wrote looks unchanged to a CPU holding a cached copy.
//
// So memory splits by who touches it. CPU-only buffers go to RAM_D1 (cached, where plain .bss
// lands) or DTCM. The SAI rings, which DMA shares, go to RAM_D2 and the MPU marks that region
// non-cacheable, which removes the coherency problem instead of cleaning and invalidating at every
// touch point. Getting this backwards works on the bench and crackles in the field.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// A buffer in RAM_D2, the non-cached DMA region. The section comes from firmware/cubevox.ld.
// Alignment is the 32-byte D-cache line, so a ring never shares a line with cached data; size the
// buffer to a multiple of 32 as well.
#define H7_DMA_MEM __attribute__((section(".sram1_bss"), aligned(32)))

// A CPU-only buffer in DTCM, which no DMA engine can reach. The section is NOLOAD and startup does
// not zero it, so call h7_dtcm_bss_zero() before use.
#define H7_DTCM_BSS __attribute__((section(".dtcm_bss"), aligned(32)))

// Makes RAM_D2 Normal, non-cacheable memory. Call it before any DMA buffer is used. Idempotent.
// Returns false when the region could not be programmed, which is fatal for audio: a silently
// cached DMA ring is the bug this file exists to prevent.
bool h7_block_mem_mpu_init(void);

// Zeroes the .dtcm_bss section between the bounds firmware/cubevox.ld defines.
void h7_dtcm_bss_zero(void);

// The memory an address is in.
enum H7MemRegionId {
  H7_MEM_UNKNOWN = 0,
  H7_MEM_ITCM,
  H7_MEM_DTCM,
  H7_MEM_RAM_D1,
  H7_MEM_RAM_D2,
  H7_MEM_RAM_D3,
  H7_MEM_FLASH,
};

H7MemRegionId h7_block_mem_region_of(const void* addr);
