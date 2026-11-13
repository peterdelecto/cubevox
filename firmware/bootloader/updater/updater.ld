/* The updater, linked at the application start so every bootloader takes it as an application
 * image. Addresses mirror boot_block.h; each carries the macro's name in a comment.
 *
 * Flash, bottom to top: the boot vectors, the image header at +0x400, the RAM code's load image,
 * the code, the .data load image, the carried bootloader last.
 * DTCM: .data, .bss and the stack, below the boot block.
 * AXI SRAM: the RAM vectors and code (updater_ram.o), then the two sector 0 images. */

MEMORY
{
  /* CV_APP_START .. CV_APP_END */
  FLASH   (rx)  : ORIGIN = 0x08020000, LENGTH = 0x000E0000
  /* up to CV_BOOT_BLOCK_ADDR */
  DTCM    (rw)  : ORIGIN = 0x20000000, LENGTH = 0x0001FF00
  AXISRAM (rwx) : ORIGIN = 0x24000000, LENGTH = 512K
}

_estack = ORIGIN(DTCM) + LENGTH(DTCM);
_stack_size = 0x4000;

ENTRY(Reset_Handler)

SECTIONS
{
  .isr_vector :
  {
    KEEP(*(.isr_vector))
  } > FLASH

  /* struct CvImageHeader at CV_APP_START + CV_IMAGE_HEADER_OFFSET */
  .cv_image_hdr : ALIGN(0x400)
  {
    KEEP(*(.cv_image_hdr))
  } > FLASH
  ASSERT(ADDR(.cv_image_hdr) == 0x08020400, "updater.ld: the image header is not at +0x400")

  /* Everything updater_ram.o holds runs from AXI SRAM, its constants included. */
  .ramfunc : ALIGN(1024)
  {
    _sramfunc = .;
    KEEP(*(.cv_ram_vectors))
    *updater_ram.o(.text .text.* .rodata .rodata.* .data .data.*)
    . = ALIGN(4);
    _eramfunc = .;
  } > AXISRAM AT > FLASH
  _siramfunc = LOADADDR(.ramfunc);

  .text :
  {
    . = ALIGN(4);
    *(.text .text.*)
    *(.rodata .rodata.*)
    *(.ARM.extab* .gnu.linkonce.armextab.*)
    . = ALIGN(4);
    __init_array_start = .;
    KEEP(*(.init_array .init_array.*))
    __init_array_end = .;
  } > FLASH

  .ARM.exidx :
  {
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
  } > FLASH

  .data : ALIGN(4)
  {
    _sdata = .;
    *(.data .data.*)
    . = ALIGN(4);
    _edata = .;
  } > DTCM AT > FLASH
  _sidata = LOADADDR(.data);

  .bss (NOLOAD) : ALIGN(4)
  {
    _sbss = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > DTCM
  ASSERT(_ebss + _stack_size <= _estack, "updater.ld: .bss leaves no room for the stack")

  .cv_sector0_image (NOLOAD) : ALIGN(32)
  {
    *(.cv_sector0_image)
  } > AXISRAM

  /* The carried bootloader, last, so its bytes end where the image ends. */
  .cv_payload : ALIGN(16)
  {
    KEEP(*(.cv_payload))
  } > FLASH
}
