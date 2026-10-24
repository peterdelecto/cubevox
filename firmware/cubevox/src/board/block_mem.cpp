// Adapted from Teensy-H7-Port, lib/platform/h7/h7_block_mem.cpp. See block_mem.h for the design.

#include "board/block_mem.h"

#include <Arduino.h>

namespace {

struct H7MemRegion {
  H7MemRegionId id;
  uint32_t base;
  uint32_t size;
};

// The STM32H743 memory map as the stock linker script declares it.
const H7MemRegion kRegions[] = {
    {H7_MEM_ITCM, 0x00000000, 64 * 1024},
    {H7_MEM_DTCM, 0x20000000, 128 * 1024},
    {H7_MEM_RAM_D1, 0x24000000, 512 * 1024},
    {H7_MEM_RAM_D2, 0x30000000, 288 * 1024},
    {H7_MEM_RAM_D3, 0x38000000, 64 * 1024},
    {H7_MEM_FLASH, 0x08000000, 2 * 1024 * 1024},  // two banks of 8 sectors of 128 KB
};

bool mpuReady = false;

// Lowest MPU region number that is not already enabled, or -1 when all 16 are taken. Reading RASR
// needs the region selected through RNR first.
int firstFreeMpuRegion() {
  for (int n = 0; n < 16; n++) {
    MPU->RNR = static_cast<uint32_t>(n);
    if ((MPU->RASR & MPU_RASR_ENABLE_Msk) == 0) return n;
  }
  return -1;
}

}  // namespace

H7MemRegionId h7_block_mem_region_of(const void* addr) {
  const uint32_t a = reinterpret_cast<uint32_t>(addr);
  for (const H7MemRegion& r : kRegions) {
    if (a >= r.base && a - r.base < r.size) return r.id;
  }
  return H7_MEM_UNKNOWN;
}

bool h7_block_mem_mpu_init(void) {
  if (mpuReady) return true;

  // The D2 SRAM has its own AHB2ENR clock gates, and STM32duino's startup leaves them off. Without
  // them the MPU region below is programmed correctly over unclocked SRAM, and any access can
  // fault or read garbage. The third gate is guarded because H72x/H73x have two D2 blocks and
  // define no D2SRAM3EN.
  __HAL_RCC_D2SRAM1_CLK_ENABLE();
  __HAL_RCC_D2SRAM2_CLK_ENABLE();
#if defined(__HAL_RCC_D2SRAM3_CLK_ENABLE)
  __HAL_RCC_D2SRAM3_CLK_ENABLE();
#endif

  const int n = firstFreeMpuRegion();
  if (n < 0) return false;

  MPU_Region_InitTypeDef r = {};

  HAL_MPU_Disable();

  r.Enable = MPU_REGION_ENABLE;
  r.Number = static_cast<uint8_t>(n);
  r.BaseAddress = 0x30000000;

  // A region is a power of two at least as large as the RAM it covers. RAM_D2 is 288 KB on the
  // H74x and H75x, so 512 KB is the smallest that fits. The H72x and H73x have 32 KB.
#if defined(__HAL_RCC_D2SRAM3_CLK_ENABLE)
  r.Size = MPU_REGION_SIZE_512KB;
#else
  r.Size = MPU_REGION_SIZE_32KB;
#endif

  // TEX level 1 with C and B clear is Normal, non-cacheable memory. Device or Strongly Ordered
  // would also forbid the unaligned and speculative accesses ordinary C code makes.
  r.TypeExtField = MPU_TEX_LEVEL1;
  r.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  r.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  r.IsShareable = MPU_ACCESS_SHAREABLE;

  r.AccessPermission = MPU_REGION_FULL_ACCESS;
  r.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;  // data only
  r.SubRegionDisable = 0x00;

  HAL_MPU_ConfigRegion(&r);

  // PRIVILEGED_DEFAULT keeps the default memory map for every address outside this region.
  // Without it, enabling the MPU would make all other RAM inaccessible.
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

  // Lines cached under the old attributes could still be written back over what DMA puts here.
  SCB_CleanInvalidateDCache();

  // Read the region back instead of trusting the write. ENABLE says it took and C clear is the
  // property this file exists for.
  MPU->RNR = static_cast<uint32_t>(n);
  const uint32_t rasr = MPU->RASR;
  const bool enabled = (rasr & MPU_RASR_ENABLE_Msk) != 0;
  const bool notCached = (rasr & MPU_RASR_C_Msk) == 0;

  mpuReady = enabled && notCached;
  return mpuReady;
}

// The bounds come from firmware/cubevox.ld. Weak, so a link without it leaves both null and the
// zeroing does nothing.
extern "C" uint32_t __dtcm_bss_start __attribute__((weak));
extern "C" uint32_t __dtcm_bss_end __attribute__((weak));

void h7_dtcm_bss_zero(void) {
  uint32_t* p = &__dtcm_bss_start;
  uint32_t* end = &__dtcm_bss_end;
  if (p == nullptr || end == nullptr) return;
  while (p < end) *p++ = 0;
}
