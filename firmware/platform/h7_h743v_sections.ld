/* Source: /Users/j/Documents/Claude/Projects/Teensy-H7-Port/lib/platform/h7/h7_h743v_sections.ld (copied verbatim below; do not edit here) */
/* h7_h743v_sections.ld — the port's two extra sections, for STM32duino's
 * H743V linker script (GENERIC_H743VITX and the WeAct board).
 *
 * Part of Teensy-H7-Port. The stock script declares RAM_D2 and DTCM and
 * places nothing in either; every section lands in RAM_D1. The Daisy script
 * already maps .sram1_bss to RAM_D2, so this file is for the H743V build only.
 * Linking it on the Daisy would give .sram1_bss a second output section.
 *
 * Passed with --script, the INSERT makes this augment the stock script, the
 * way STM32duino's own system/ldscript.ld adds .noinit. This script is read
 * before the stock one and cannot name its regions, so addresses are explicit
 * and each ASSERT does the region's overflow check.
 *
 * Both sections are NOLOAD, as on the Daisy: startup zeroes neither.
 * h7_dtcm_bss_zero() zeroes .dtcm_bss. */

SECTIONS
{
  /* H7_DMA_MEM: DMA rings, MPU-marked non-cacheable (h7_block_mem.h). */
  .sram1_bss 0x30000000 (NOLOAD) :
  {
    . = ALIGN(32);
    __sram1_bss_start = .;
    *(.sram1_bss)
    *(.sram1_bss*)
    . = ALIGN(32);
    __sram1_bss_end = .;
  }
  ASSERT(__sram1_bss_end <= 0x30048000, "h7_h743v_sections.ld: .sram1_bss overflows the 288 KB RAM_D2")

  /* H7_DTCM_BSS: CPU-only buffers. DMA cannot reach DTCM. */
  .dtcm_bss 0x20000000 (NOLOAD) :
  {
    . = ALIGN(32);
    __dtcm_bss_start = .;
    *(.dtcm_bss)
    *(.dtcm_bss*)
    . = ALIGN(32);
    __dtcm_bss_end = .;
  }
  ASSERT(__dtcm_bss_end <= 0x20020000, "h7_h743v_sections.ld: .dtcm_bss overflows the 128 KB DTCM")
}
INSERT AFTER .bss;
