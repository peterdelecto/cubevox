// The bootloader the updater carries: struct CvPayloadHeader (updater_check.h), then the bytes of
// the bootloader .bin. build.sh passes the file and its length and CRC-32; updater.ld puts this
// section last in the image, so the bytes end where the file ends.

  .syntax unified
  .section .cv_payload, "a"
  .balign 16
  .global cvPayloadHeader
cvPayloadHeader:
  .word 0x4C425643            // CV_PAYLOAD_MAGIC, the bytes C V B L
  .word CV_PAYLOAD_LENGTH
  .word CV_PAYLOAD_CRC
  .word 0
  .global cvPayloadBytes
cvPayloadBytes:
  .incbin CV_PAYLOAD_BIN
  .global cvPayloadEnd
cvPayloadEnd:
