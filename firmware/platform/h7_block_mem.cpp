// Source: /Users/j/Documents/Claude/Projects/Teensy-H7-Port/lib/platform/h7/h7_block_mem.cpp (copied verbatim below; do not edit here)
/* h7_block_mem.cpp — see h7_block_mem.h for the design and the reasoning. */

#include "h7_block_mem.h"

#include <Arduino.h>

/* Memory map of the STM32H750 as the Daisy Seed linker script declares it
 * (DAISY_SEED.ld). Kept in one table so region naming and DMA-reachability
 * cannot disagree with each other. */
struct H7MemRegion {
	H7MemRegionId id;
	uint32_t      base;
	uint32_t      size;
	const char   *name;
	bool          dma_reachable;
};

/* One table, three answers: identity, name and DMA reachability. They cannot
 * drift apart because there is nowhere for them to drift to. */
static const H7MemRegion kRegions[] = {
	/* ITCM and DTCM are core-local. They hang off the tightly coupled buses,
	 * which no DMA master is connected to — hence dma_reachable = false. This
	 * is the single most surprising line in the file. */
	{H7_MEM_ITCM,   0x00000000,       64 * 1024, "ITCM",   false},
	{H7_MEM_DTCM,   0x20000000,      128 * 1024, "DTCM",   false},
	{H7_MEM_RAM_D1, 0x24000000,      512 * 1024, "RAM_D1", true},
	{H7_MEM_RAM_D2, 0x30000000,      288 * 1024, "RAM_D2", true},
	{H7_MEM_RAM_D3, 0x38000000,       64 * 1024, "RAM_D3", true},
	{H7_MEM_FLASH,  0x08000000,      128 * 1024, "FLASH",  true},
	{H7_MEM_SDRAM,  0xC0000000, 64 * 1024 * 1024, "SDRAM", true},
};

static const H7MemRegion *lookup(const void *addr)
{
	uint32_t a = (uint32_t)addr;
	for (unsigned i = 0; i < sizeof(kRegions) / sizeof(kRegions[0]); i++) {
		const H7MemRegion *r = &kRegions[i];
		if (a >= r->base && a < r->base + r->size) return r;
	}
	return nullptr;
}

H7MemRegionId h7_block_mem_region_of(const void *addr)
{
	const H7MemRegion *r = lookup(addr);
	return r ? r->id : H7_MEM_UNKNOWN;
}

const char *h7_block_mem_region_name(const void *addr)
{
	const H7MemRegion *r = lookup(addr);
	return r ? r->name : "?";
}

bool h7_block_mem_dma_reachable(const void *addr)
{
	const H7MemRegion *r = lookup(addr);
	return r ? r->dma_reachable : false;
}

static bool mpu_ready = false;
static int  mpu_region = -1;

bool h7_block_mem_mpu_ready(void)
{
	return mpu_ready;
}

int h7_block_mem_mpu_region(void)
{
	return mpu_region;
}

/* Lowest MPU region number that is not already enabled, or -1 if all 16 are
 * taken. Reading RASR requires selecting the region through RNR first. */
static int first_free_mpu_region(void)
{
	for (int n = 0; n < 16; n++) {
		MPU->RNR = (uint32_t)n;
		if ((MPU->RASR & MPU_RASR_ENABLE_Msk) == 0) return n;
	}
	return -1;
}

bool h7_block_mem_mpu_init(void)
{
	if (mpu_ready) return true;

	/* D2 domain SRAM has its own AHB2ENR clock gate, separate from the D1
	 * (RAM_D1/AXI) and D3 domains, and it is not enabled by STM32duino's
	 * startup code. Without this the MPU region below is programmed
	 * correctly but the underlying SRAM macrocells are unclocked, so any
	 * access through them can fault or read garbage. RM0433 documents
	 * D2SRAM1EN/D2SRAM2EN/D2SRAM3EN in RCC_AHB2ENR as the D2-domain SRAM
	 * clock gates; libDaisy's System::Init() enables the same bits for the
	 * same reason.
	 *
	 * The third gate is guarded because it is not universal: H74x/H75x have
	 * three D2 SRAM blocks (128 + 128 + 32 = 288 KB), while H72x/H73x have
	 * two (16 + 16 = 32 KB) and define no D2SRAM3EN at all. Found by
	 * building this file for GENERIC_H723ZGTX, 2026-09-07. */
	__HAL_RCC_D2SRAM1_CLK_ENABLE();
	__HAL_RCC_D2SRAM2_CLK_ENABLE();
#if defined(__HAL_RCC_D2SRAM3_CLK_ENABLE)
	__HAL_RCC_D2SRAM3_CLK_ENABLE();
#endif

	int n = first_free_mpu_region();
	if (n < 0) return false;

	MPU_Region_InitTypeDef r = {0};

	HAL_MPU_Disable();

	r.Enable      = MPU_REGION_ENABLE;
	r.Number      = (uint8_t)n;
	r.BaseAddress = 0x30000000;

	/* MPU regions must be a power of two and at least as large as the RAM
	 * they cover. RAM_D2 is 288 KB on H74x/H75x, so 512 KB is the smallest
	 * that fits; it is 32 KB on H72x/H73x, which needs exactly 32 KB. The
	 * same D2SRAM3 test as above distinguishes them. Sizing per chip rather
	 * than always taking 512 KB keeps the region off half a megabyte of
	 * unmapped address space on the smaller parts, which matters because
	 * Normal memory (what this region is) permits speculative access. The
	 * H74x/H75x case still over-covers by 224 KB, unchanged and verified on
	 * the Daisy. */
#if defined(__HAL_RCC_D2SRAM3_CLK_ENABLE)
	r.Size = MPU_REGION_SIZE_512KB;
#else
	r.Size = MPU_REGION_SIZE_32KB;
#endif

	/* TEX level 1 with C and B both clear is "Normal memory, non-cacheable".
	 * Not Device or Strongly Ordered: those would also forbid the unaligned
	 * and speculative accesses that ordinary C code makes, and would slow the
	 * CPU side of the ring for no benefit. Non-cacheable Normal is exactly the
	 * property wanted and nothing more. */
	r.TypeExtField    = MPU_TEX_LEVEL1;
	r.IsCacheable     = MPU_ACCESS_NOT_CACHEABLE;
	r.IsBufferable    = MPU_ACCESS_NOT_BUFFERABLE;
	r.IsShareable     = MPU_ACCESS_SHAREABLE;

	r.AccessPermission = MPU_REGION_FULL_ACCESS;
	r.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;  /* data only */
	r.SubRegionDisable = 0x00;

	HAL_MPU_ConfigRegion(&r);

	/* MPU_PRIVILEGED_DEFAULT keeps the default memory map for every address
	 * outside region 0. Without it, enabling the MPU would make all other RAM
	 * inaccessible and the next instruction fetch would fault. */
	HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

	/* Anything already sitting in cache for this region was cached under the
	 * old attributes. Drop it, or a stale line could still be written back
	 * over what DMA puts there. */
	SCB_CleanInvalidateDCache();

	/* Read the region back rather than trusting the write. Selecting the region
	 * through RNR makes its RASR readable; ENABLE says it took, and C clear is
	 * the property the whole file exists for. */
	MPU->RNR = (uint32_t)n;
	uint32_t rasr = MPU->RASR;
	bool enabled    = (rasr & MPU_RASR_ENABLE_Msk) != 0;
	bool not_cached = (rasr & MPU_RASR_C_Msk) == 0;

	mpu_ready = enabled && not_cached;
	mpu_region = mpu_ready ? n : -1;

	return mpu_ready;
}

/* Bounds come from h7_h743v_sections.ld. Weak, so a link without that
 * script leaves both at 0 and the zeroing does nothing. */
extern "C" uint32_t __dtcm_bss_start __attribute__((weak));
extern "C" uint32_t __dtcm_bss_end __attribute__((weak));

void h7_dtcm_bss_zero(void)
{
	uint32_t *p = &__dtcm_bss_start;
	uint32_t *end = &__dtcm_bss_end;
	if (p == NULL || end == NULL) return;
	while (p < end) *p++ = 0;
}
