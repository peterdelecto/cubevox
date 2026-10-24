// Runs the bootloader's image check (boards/cubevox/image_check.c) on files, so build.sh --selftest
// can hold it against tools/stamp_image.py. A file is read as flash: its bytes, then erased 0xFF
// to the end of the application region.
//
//   imagecheck check FILE                  prints "good <version>", "bad <version>" or "none"
//   imagecheck fake FILE BYTES [VERSION]   writes an unstamped image: body bytes, the header with
//                                          length and CRC zero, header version VERSION (default
//                                          CV_IMAGE_HEADER_VERSION), release name v2.00.10
//   imagecheck flip IN OUT INDEX           writes IN with bit 0 of byte INDEX inverted

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_block.h"
#include "image_check.h"

#define CV_REGION_BYTES (CV_APP_END - CV_APP_START)

static uint8_t gImage[CV_REGION_BYTES];

static int fail(const char* what, const char* where) {
  fprintf(stderr, "[ERROR] %s %s\n", what, where);
  return 1;
}

// Reads up to the region size; returns the byte count, or -1.
static long readImage(const char* path) {
  FILE* file = fopen(path, "rb");
  if (file == NULL) return -1;
  memset(gImage, 0xFF, sizeof gImage);
  const size_t n = fread(gImage, 1, sizeof gImage, file);
  const int extra = fgetc(file);
  fclose(file);
  return extra == EOF ? (long)n : -1;
}

static int writeImage(const char* path, uint32_t length) {
  FILE* file = fopen(path, "wb");
  if (file == NULL) return fail("cannot open", path);
  const size_t n = fwrite(gImage, 1, length, file);
  return fclose(file) == 0 && n == length ? 0 : fail("cannot write", path);
}

static int check(const char* path) {
  if (readImage(path) < 0) return fail("cannot read, or larger than the region:", path);
  struct CvImageReport report;
  cvImageInspect(gImage, CV_REGION_BYTES, &report);
  if (report.state == CvImageNone) {
    printf("none\n");
  } else {
    printf("%s %s\n", report.state == CvImageGood ? "good" : "bad", report.version);
  }
  return 0;
}

// The layout of tools/stamp_image.py's fake_image: an odd-period body and the header at its offset.
static int fake(const char* path, const char* bytesText, uint32_t headerVersion) {
  const unsigned long length = strtoul(bytesText, NULL, 0);
  if (length < CV_IMAGE_HEADER_OFFSET + CV_IMAGE_HEADER_SIZE || length > CV_REGION_BYTES) {
    return fail("image size out of range:", bytesText);
  }
  for (unsigned long i = 0; i < length; ++i) gImage[i] = (uint8_t)(i % 251u);
  struct CvImageHeader header;
  memset(&header, 0, sizeof header);
  header.magic = CV_IMAGE_MAGIC;
  header.headerVersion = headerVersion;
  memcpy(header.version, "v2.00.10", 8);
  memcpy(gImage + CV_IMAGE_HEADER_OFFSET, &header, sizeof header);
  return writeImage(path, (uint32_t)length);
}

static int flip(const char* in, const char* out, const char* indexText) {
  const long length = readImage(in);
  if (length < 0) return fail("cannot read", in);
  const unsigned long index = strtoul(indexText, NULL, 0);
  if (index >= (unsigned long)length) return fail("index past the image:", indexText);
  gImage[index] ^= 0x01u;
  return writeImage(out, (uint32_t)length);
}

int main(int argc, char** argv) {
  if (argc == 3 && strcmp(argv[1], "check") == 0) return check(argv[2]);
  if (argc == 4 && strcmp(argv[1], "fake") == 0) return fake(argv[2], argv[3], CV_IMAGE_HEADER_VERSION);
  if (argc == 5 && strcmp(argv[1], "fake") == 0) {
    return fake(argv[2], argv[3], (uint32_t)strtoul(argv[4], NULL, 0));
  }
  if (argc == 5 && strcmp(argv[1], "flip") == 0) return flip(argv[2], argv[3], argv[4]);
  return fail("usage: imagecheck check FILE | fake FILE BYTES [VERSION] | flip IN OUT INDEX", "");
}
