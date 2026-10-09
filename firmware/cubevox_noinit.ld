/* cubevox_noinit.ld: the fault record's section, at the top of DTCM.
 *
 * The stock .noinit lands at 0x20000000, where platform/h7_h743v_sections.ld also
 * starts .dtcm_bss, so the record shared bytes with the pitch stage state and its
 * constructor wiped the record before faultInit() read it. This section sits above
 * .dtcm_bss; nothing zeroes it, and a system reset keeps DTCM. */

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
