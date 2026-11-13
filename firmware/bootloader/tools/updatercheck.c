// Runs the updater's payload check (updater/updater_check.c) on files, so build.sh can hold it
// against the built bootloader and the built bootloader file.
//
//   updatercheck payload BOOTLOADER.bin   checks a payload built from the file, then lying ones: a
//                                         wrong length both ways, a wrong CRC, a payload over the
//                                         sector, no magic, an application in place of a bootloader
//   updatercheck image UPDATER.bin BOOTLOADER.bin
//                                         finds the payload the updater image ends with, checks it
//                                         and compares its bytes with BOOTLOADER.bin

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image_check.h"
#include "updater_check.h"

// Room for an image the size of the application region.
#define CV_FILE_MAX (CV_APP_END - CV_APP_START)

static uint8_t gFile[CV_FILE_MAX];
static uint8_t gBoot[CV_FILE_MAX];
static uint8_t gWork[CV_FLASH_SECTOR_SIZE + 64u];

static int fail(const char* what, const char* where) {
  fprintf(stderr, "[ERROR] %s %s\n", what, where);
  return 1;
}

// Reads a whole file of at most `size` bytes; returns the byte count, or -1.
static long readFile(const char* path, uint8_t* out, uint32_t size) {
  FILE* file = fopen(path, "rb");
  if (file == NULL) return -1;
  const size_t n = fread(out, 1, size, file);
  const int extra = fgetc(file);
  fclose(file);
  return extra == EOF ? (long)n : -1;
}

static struct CvPayloadHeader headerFor(const uint8_t* bytes, uint32_t length) {
  const struct CvPayloadHeader header = {CV_PAYLOAD_MAGIC, length, cvCrc32(0u, bytes, length), 0u};
  return header;
}

// One case: the check of `header` over `available` bytes must give `want`.
static int expect(const char* what, const struct CvPayloadHeader* header, const uint8_t* bytes, uint32_t available,
                  CvPayloadState want) {
  const CvPayloadState got = cvPayloadCheck(header, bytes, available);
  if (got != want) {
    fprintf(stderr, "[ERROR] payload %s: the check says '%s', expected '%s'\n", what, cvPayloadStateName(got),
            cvPayloadStateName(want));
    return 1;
  }
  printf("[selftest] payload %s: %s\n", what, cvPayloadStateName(got));
  return 0;
}

static void putWord(uint8_t* bytes, uint32_t index, uint32_t value) {
  memcpy(bytes + 4u * index, &value, sizeof value);
}

static int payloadCases(const char* path) {
  const long n = readFile(path, gBoot, sizeof gBoot);
  if (n <= 0) return fail("cannot read", path);
  const uint32_t length = (uint32_t)n;
  if (length > CV_FLASH_SECTOR_SIZE) return fail("is larger than sector 0:", path);
  struct CvPayloadHeader header = headerFor(gBoot, length);
  int bad = expect("built bootloader", &header, gBoot, length, CvPayloadGood);

  struct CvPayloadHeader lying = header;
  lying.length = length + 32u;
  bad |= expect("length over the bytes carried", &lying, gBoot, length, CvPayloadLengthWrong);
  lying.length = length - 32u;
  bad |= expect("length under the bytes carried", &lying, gBoot, length, CvPayloadLengthWrong);
  lying.length = 0u;
  bad |= expect("length zero", &lying, gBoot, 0u, CvPayloadLengthWrong);
  lying = header;
  lying.crc32 ^= 1u;
  bad |= expect("wrong CRC", &lying, gBoot, length, CvPayloadCrcWrong);
  lying = header;
  lying.magic = 0u;
  bad |= expect("no magic", &lying, gBoot, length, CvPayloadNoMagic);

  // A flipped byte inside the bytes, as a damaged file carries.
  memcpy(gWork, gBoot, length);
  gWork[length / 2u] ^= 0x01u;
  bad |= expect("flipped byte", &header, gWork, length, CvPayloadCrcWrong);

  // Over the sector with a length and a CRC that agree.
  const uint32_t big = CV_FLASH_SECTOR_SIZE + 32u;
  memset(gWork, 0xFF, big);
  memcpy(gWork, gBoot, length);
  lying = headerFor(gWork, big);
  bad |= expect("over sector 0", &lying, gWork, big, CvPayloadTooLarge);

  // An application image: its reset handler lies in the application region, not in sector 0.
  memcpy(gWork, gBoot, length);
  putWord(gWork, 1u, CV_APP_START + 0x401u);
  lying = headerFor(gWork, length);
  bad |= expect("application vectors", &lying, gWork, length, CvPayloadNotBootloader);
  putWord(gWork, 1u, 0x08000201u);
  putWord(gWork, 0u, 0x24080000u);
  lying = headerFor(gWork, length);
  bad |= expect("stack outside DTCM", &lying, gWork, length, CvPayloadNotBootloader);

  if (bad == 0) printf("[selftest] PASS: the updater's check refuses wrong lengths, a wrong CRC and an oversize payload\n");
  return bad;
}

// The header the image ends with: the one whose length reaches exactly to the end of the file.
static int imageCase(const char* updater, const char* bootloader) {
  const long n = readFile(updater, gFile, sizeof gFile);
  if (n <= 0) return fail("cannot read", updater);
  const long b = readFile(bootloader, gBoot, sizeof gBoot);
  if (b <= 0) return fail("cannot read", bootloader);
  const uint32_t size = (uint32_t)n;
  for (uint32_t at = 0u; at + sizeof(struct CvPayloadHeader) <= size; at += 4u) {
    struct CvPayloadHeader header;
    memcpy(&header, gFile + at, sizeof header);
    const uint32_t available = size - at - (uint32_t)sizeof header;
    if (header.magic != CV_PAYLOAD_MAGIC || header.length != available) continue;
    const uint8_t* bytes = gFile + at + sizeof header;
    const CvPayloadState state = cvPayloadCheck(&header, bytes, available);
    if (state != CvPayloadGood) return fail(cvPayloadStateName(state), updater);
    if ((uint32_t)b != header.length || memcmp(bytes, gBoot, header.length) != 0) {
      return fail("carries other bytes than", bootloader);
    }
    printf("good %u bytes crc %08x at 0x%08x\n", (unsigned)header.length, (unsigned)header.crc32,
           (unsigned)(CV_APP_START + at + sizeof header));
    return 0;
  }
  return fail("ends with no payload", updater);
}

int main(int argc, char** argv) {
  if (argc == 3 && strcmp(argv[1], "payload") == 0) return payloadCases(argv[2]);
  if (argc == 4 && strcmp(argv[1], "image") == 0) return imageCase(argv[2], argv[3]);
  fprintf(stderr, "[ERROR] usage: updatercheck payload BOOTLOADER.bin | image UPDATER.bin BOOTLOADER.bin\n");
  return 1;
}
