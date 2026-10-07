// Source: /Users/j/Documents/Claude/Projects/Teensy-H7-Port/lib/platform/h7/h7_block_mem.h (copied verbatim below; do not edit here)
/* h7_block_mem.h — where audio memory lives on an STM32H7, and why.
 *
 * Part of Teensy-H7-Port. ROADMAP.md calls this hal/h7_block_mem.c.
 *
 * This is the H7 problem with no Teensy analog. Two facts drive everything:
 *
 *   1. DMA1 and DMA2 cannot reach DTCM. The fastest RAM on the chip is
 *      invisible to them.
 *   2. The D-cache will lie to a DMA engine. A buffer the CPU has touched sits
 *      dirty in cache while DMA reads stale memory underneath, and a buffer
 *      DMA has written looks unchanged to a CPU holding a cached copy.
 *
 * So audio memory splits in two, by who touches it:
 *
 *   AUDIO BLOCK POOL -> RAM_D1 (AXI SRAM, 0x24000000, 512 KB), cached.
 *       Only the CPU ever touches blocks; I/O drivers copy in and out of their
 *       own buffers. So the pool wants to be big and fast, and caching is
 *       purely a win. This is where .bss already lands on a Daisy Seed
 *       (DAISY_SEED.ld: `.bss ... >RAM_D1`), so the pool needs no attribute.
 *
 *   DMA RING BUFFERS -> RAM_D2 (0x30000000, 288 KB), MPU-marked non-cached.
 *       Small — the SAI ring is about 1 KB — and shared with a DMA engine.
 *       Marking the region non-cached removes the coherency problem outright
 *       instead of sprinkling clean/invalidate calls at every touch point and
 *       hoping none were missed.
 *
 * Getting this backwards is the classic H7 bug: it works on the bench and
 * crackles in the field, because a cache hit that happened to be correct is
 * indistinguishable from a correct design until timing shifts.
 */

#ifndef h7_block_mem_h_
#define h7_block_mem_h_

#include <stdint.h>
#include <stdbool.h>

/* Put a buffer in RAM_D2, the non-cached DMA region.
 *
 * The section name comes from the Daisy Seed linker script, which already
 * provides `.sram1_bss (NOLOAD) ... >RAM_D2`. The H743V script has no such
 * section and would put the ring in RAM_D1, so that build links
 * h7_h743v_sections.ld as well.
 *
 * 32-byte alignment is not cosmetic: it is the D-cache line size. A buffer
 * sharing a cache line with cached data either side of it would drag that
 * neighbour into whatever the DMA does, so every DMA buffer starts and ends on
 * a line boundary. Size the buffer to a multiple of 32 as well. */
#define H7_DMA_MEM __attribute__((section(".sram1_bss"), aligned(32)))

/* Put a CPU-only buffer in DTCM (0x20000000, 128 KB), the fastest data RAM.
 *
 * DMA cannot reach it, so only buffers no DMA engine ever touches belong here.
 * STM32duino's H743V linker script maps nothing to DTCM, so the section needs
 * h7_h743v_sections.ld on the link line (test/build_fxbox_h7.sh adds it).
 * The section is NOLOAD: startup does not zero it, so call h7_dtcm_bss_zero()
 * before use. */
#define H7_DTCM_BSS __attribute__((section(".dtcm_bss"), aligned(32)))

/* Where a granular engine's sample bank lives. CPU-only, never DMA'd.
 *
 * Default is AXI SRAM (RAM_D1), where plain .bss already lands, so the
 * attribute is empty. Define H7_GRAIN_BANK_IN_DTCM to move the bank to DTCM,
 * which frees AXI for the audio pool but caps the bank near 100 KB. */
#if defined(H7_GRAIN_BANK_IN_DTCM)
  #define H7_GRAIN_BANK_SECTION H7_DTCM_BSS
#else
  #define H7_GRAIN_BANK_SECTION
#endif

/* Configure the MPU so RAM_D2 is Normal, non-cacheable, and call it before any
 * DMA buffer is used. Idempotent. Returns false if the region could not be
 * programmed, which must be treated as fatal for audio — a silently cached DMA
 * ring is exactly the bug this file exists to prevent. */
bool h7_block_mem_mpu_init(void);

/* Zero the .dtcm_bss section. A no-op when h7_h743v_sections.ld is not linked. */
void h7_dtcm_bss_zero(void);

/* True once h7_block_mem_mpu_init() has succeeded. */
bool h7_block_mem_mpu_ready(void);

/* Which MPU region number was claimed, or -1 if none has been. The region is
 * chosen at run time from whichever ones are still free, rather than hardcoded:
 * STM32duino 3.0.0 programs no MPU regions at all (checked across cores/,
 * system/ and the variant), but silently overwriting someone else's region if
 * that ever changes would be a miserable bug to find. */
int h7_block_mem_mpu_region(void);

/* Which RAM an address is in. Callers that need to make a decision use this;
 * the name function is for printing. Keeping them separate stops region
 * identity from being re-derived by picking apart a display string. */
enum H7MemRegionId {
	H7_MEM_UNKNOWN = 0,
	H7_MEM_ITCM,
	H7_MEM_DTCM,
	H7_MEM_RAM_D1,
	H7_MEM_RAM_D2,
	H7_MEM_RAM_D3,
	H7_MEM_FLASH,
	H7_MEM_SDRAM,
};

H7MemRegionId h7_block_mem_region_of(const void *addr);

/* The same region as a short static string, for reports: "DTCM", "RAM_D1", … ,
 * or "?" for an address in no known region. */
const char *h7_block_mem_region_name(const void *addr);

/* True if a DMA engine can reach this address at all. DTCM is the trap: it is
 * perfectly good RAM that DMA simply cannot see. */
bool h7_block_mem_dma_reachable(const void *addr);

#endif
