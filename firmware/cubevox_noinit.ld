/* cubevox_noinit.ld: the fault record's section, at the top of DTCM.
 *
 * The section sits above .dtcm_bss and nothing zeroes it, so the record survives a system reset. */

SECTIONS
{
  .cv_noinit 0x2001FFC0 (NOLOAD) :
  {
    __cv_noinit_start = .;
    KEEP(*(.cv_noinit))
    __cv_noinit_end = .;
  }
  ASSERT(__cv_noinit_end <= 0x20020000, "cubevox_noinit.ld: .cv_noinit overflows DTCM")
  ASSERT(__dtcm_bss_end <= __cv_noinit_start, "cubevox_noinit.ld: .dtcm_bss runs into the fault record")
}
INSERT AFTER .bss;
