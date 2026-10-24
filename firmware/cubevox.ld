/* cubevox.ld: the sections cubevox adds to the core's linker script for the H743V.
 *
 * Passed with --script, an INSERT makes the block before it augment the stock script, the way
 * STM32duino's own system/ldscript.ld adds .noinit. This script is read before the stock one and
 * cannot name its regions, so the RAM sections carry explicit addresses and each ASSERT does its
 * region's overflow check.
 *
 * ld cannot include board/boot_block.h. Each address that mirrors it carries the macro's name in a
 * comment, and build.sh compares the linked symbols with the header, so a drift fails the build.
 *
 * DTCM, bottom to top: .dtcm_bss, .cv_boot_block (192 bytes), .cv_noinit (64 bytes).
 * Flash, bottom to top: .isr_vector, gap, .cv_image_hdr at +0x400, code, constants, .data's load image. */

SECTIONS
{
  /* The DMA rings (H7_DMA_MEM), which the MPU marks non-cacheable (board/block_mem.h). All four
   * data sections below are NOLOAD, and startup zeroes none of them. */
  .sram1_bss 0x30000000 (NOLOAD) :
  {
    . = ALIGN(32);
    __sram1_bss_start = .;
    *(.sram1_bss)
    *(.sram1_bss*)
    . = ALIGN(32);
    __sram1_bss_end = .;
  }
  ASSERT(__sram1_bss_end <= 0x30048000, "cubevox.ld: .sram1_bss overflows the 288 KB RAM_D2")

  /* The CPU-only buffers (H7_DTCM_BSS). DMA cannot reach DTCM, and h7_dtcm_bss_zero() zeroes the
   * section. */
  .dtcm_bss 0x20000000 (NOLOAD) :
  {
    . = ALIGN(32);
    __dtcm_bss_start = .;
    *(.dtcm_bss)
    *(.dtcm_bss*)
    . = ALIGN(32);
    __dtcm_bss_end = .;
  }
  /* CV_BOOT_BLOCK_ADDR */
  ASSERT(__dtcm_bss_end <= 0x2001FF00, "cubevox.ld: .dtcm_bss runs into the boot block")

  /* The block shared with the bootloader, struct CvBootBlock. It survives a system reset. */
  /* CV_BOOT_BLOCK_ADDR */
  .cv_boot_block 0x2001FF00 (NOLOAD) :
  {
    __cv_boot_block_start = .;
    KEEP(*(.cv_boot_block))
    __cv_boot_block_end = .;
  }
  /* CV_FAULT_RECORD_ADDR */
  ASSERT(__cv_boot_block_end <= 0x2001FFC0, "cubevox.ld: .cv_boot_block runs into the fault record")

  /* The fault record. It survives a system reset. */
  /* CV_FAULT_RECORD_ADDR */
  .cv_noinit 0x2001FFC0 (NOLOAD) :
  {
    __cv_noinit_start = .;
    KEEP(*(.cv_noinit))
    __cv_noinit_end = .;
  }
  /* CV_DTCM_END */
  ASSERT(__cv_noinit_end <= 0x20020000, "cubevox.ld: .cv_noinit overflows DTCM")
}
INSERT AFTER .bss;

SECTIONS
{
  /* The image header, struct CvImageHeader. The compiler emits length and CRC as zero and the
   * stamp step of build.sh fills them in the .bin. The section takes no address of its own: an
   * explicit address would leave the flash region's fill pointer behind it, and .text would start
   * under the header. ALIGN puts it on the first 1 KB boundary after the vector table, and the
   * ASSERT checks that boundary is the one boot_block.h names. */
  .cv_image_hdr : ALIGN(0x400)
  {
    KEEP(*(.cv_image_hdr))
  }
  /* CV_APP_START + CV_IMAGE_HEADER_OFFSET */
  ASSERT(ADDR(.cv_image_hdr) == 0x08020400, "cubevox.ld: the image header is not at app start + 0x400")
  /* CV_IMAGE_HEADER_SIZE */
  ASSERT(SIZEOF(.cv_image_hdr) == 32, "cubevox.ld: the image header is not 32 bytes")
  /* CV_APP_END: flash bank 2 starts here and holds the layout record. */
  ASSERT(LOADADDR(.data) + SIZEOF(.data) <= 0x08100000, "cubevox.ld: image reaches flash bank 2")
}
INSERT AFTER .isr_vector;
