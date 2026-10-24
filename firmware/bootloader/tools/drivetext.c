// Writes OUT_DIR/INFO_UF2.TXT and OUT_DIR/DIAG.TXT with diag.c for build.sh --selftest. The sample
// box stayed in update mode after a read fault in bank 1, holds a bus fault record with a bank 2 ECC
// error, an application summary, and refused blocks of the WeAct family.
//
//   drivetext OUT_DIR

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "diag.h"

static int writeFile(const char* dir, const char* name, const char* text, uint32_t length) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE* file = fopen(path, "wb");
  if (file == NULL) {
    fprintf(stderr, "[ERROR] cannot open %s\n", path);
    return 1;
  }
  const size_t n = fwrite(text, 1, length, file);
  if (fclose(file) != 0 || n != length) {
    fprintf(stderr, "[ERROR] cannot write %s\n", path);
    return 1;
  }
  printf("%s\n", path);
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "[ERROR] usage: drivetext OUT_DIR\n");
    return 1;
  }
  static const char kSummary[] = "v2.00.10 up 1234 s software none";
  struct CvBootBlock block;
  memset(&block, 0, sizeof block);
  block.blockVersion = CV_BOOT_BLOCK_VERSION;
  block.lastReason = CvReasonImageUnreadable;
  block.faultAddress = 0x08045000u;
  memcpy(block.appSummary, kSummary, sizeof kSummary - 1u);

  const struct CvImageReport image = {CvImageGood, "v2.00.10"};
  const struct CvFaultRecord fault = {CV_FAULT_MAGIC, 2u,          0x08021A4Cu,          0x08020F31u,
                                      0x00008200u,    0x40000000u, 0xDEADBEE0u,          0xDEADBEE0u,
                                      CV_FLASH_SR_DBECCERR, 0x0123u, 0u};
  const CvDriveState state = {&block, &image, &fault, "0123456789ABCDEF01234567", "0.21.0", 12u,
                              CV_UF2_FAMILY_WEACT};

  uint32_t infoLength = 0u;
  const char* info = cvInfoText(&state, &infoLength);
  cvDiagWrite(&state);
  if (writeFile(argv[1], "INFO_UF2.TXT", info, infoLength) != 0) return 1;
  return writeFile(argv[1], "DIAG.TXT", cvDiagText, CV_DIAG_TEXT_BYTES);
}
