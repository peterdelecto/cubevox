// Writes OUT_DIR/INFO_UF2.TXT and the DIAG*.TXT files with diag.c for build.sh --selftest and
// --proofs. DIAG.TXT is the sample box. It stayed in update mode after a read fault in bank 1, holds
// the bootloader's fault registers, a bus fault record with a bank 2 ECC error, an application
// summary, written, refused and failed blocks, a wrong file by name, and a flash log of three valid
// entries, one torn entry and blank slots. The other files cover an empty log, a log that must not
// be read (a fault in the log sector, and one elsewhere in bank 2), 40 typical entries, 40 of the
// longest lines under the widest header, and a box whose crystal failed and whose option bytes did
// not program.
//
//   drivetext OUT_DIR

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "diag.h"

#define CV_LOG_ENTRIES 40u
// OPTSR with RDP level 0 and BOR level 3, and WPSN_CUR1 with sector 0 protected.
#define CV_SAMPLE_OPTSR 0x1016AAFCu
#define CV_SAMPLE_WPSN  0xFEu

typedef struct {
  uint32_t sequence;
  uint32_t kind;
  uint32_t uptimeS;
  uint32_t resetCause;
  uint32_t bootFails;
  uint32_t lastReason;
  uint32_t faultAddress;
  uint32_t saiErrors;
  uint32_t overruns;
  const char* firmware;
  const char* fault;
  uint32_t optionBytes;
} Sample;

static uint8_t gSector[CV_DIAG_LOG_SIZE];

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

static void textSet(char* field, size_t size, const char* text) {
  const size_t length = strlen(text);
  memset(field, 0, size);
  memcpy(field, text, length < size ? length : size - 1u);
}

// An entry as the application writes it: reserved bytes 0xFF, the CRC-32 last.
static void entryWrite(uint32_t slot, const Sample* s) {
  struct CvDiagEntry e;
  memset(&e, 0xFF, sizeof e);
  e.magic = CV_DIAG_MAGIC;
  e.sequence = s->sequence;
  e.kind = s->kind;
  e.uptimeS = s->uptimeS;
  e.resetCause = s->resetCause;
  e.bootFails = s->bootFails;
  e.lastReason = s->lastReason;
  e.faultAddress = s->faultAddress;
  e.saiErrors = s->saiErrors;
  e.overruns = s->overruns;
  textSet(e.firmware, sizeof e.firmware, s->firmware);
  textSet(e.fault, sizeof e.fault, s->fault);
  textSet(e.summary, sizeof e.summary, "");
  e.optionBytes = s->optionBytes;
  e.crc32 = cvCrc32(0u, (const uint8_t*)&e, CV_DIAG_ENTRY_SIZE - 4u);
  memcpy(&gSector[slot * CV_DIAG_ENTRY_SIZE], &e, sizeof e);
}

// A power cut left the second half of the entry unprogrammed.
static void entryTear(uint32_t slot) {
  memset(&gSector[slot * CV_DIAG_ENTRY_SIZE + CV_DIAG_ENTRY_SIZE / 2u], 0xFF, CV_DIAG_ENTRY_SIZE / 2u);
}

static void sectorBlank(void) {
  memset(gSector, 0xFF, sizeof gSector);
}

// Slot 0 a watchdog boot whose option byte step timed out, slot 1 an update request, slot 2 an entry
// the power cut tore, slot 3 the boot after the update request.
static void sectorSample(void) {
  static const Sample kWatchdog = {7u,         CvDiagKindBoot, CV_DIAG_UNKNOWN_U32, 0x04000000u, 1u, CvReasonNone,
                                   0u,         0u,             3u,                  "v2.00.09",  "watchdog bus",
                                   CvOptionTimeout};
  static const Sample kRequest = {8u,         CvDiagKindUpdateRequest, 1234u, 0x01000000u, 0u, CvReasonNone,
                                  0u,         2u,                      0u,    "v2.00.10",  "software none",
                                  CvOptionAlreadySet};
  static const Sample kTorn = {9u, CvDiagKindBoot, CV_DIAG_UNKNOWN_U32, 0x01000000u, 0u, CvReasonNone,
                               0u, 0u,             0u,                  "v2.00.10",  "software none",
                               CvOptionAlreadySet};
  static const Sample kAfter = {9u, CvDiagKindBoot, CV_DIAG_UNKNOWN_U32, 0x01000000u, 0u, CvReasonAppRequest,
                                0u, 0u,             0u,                  "v2.00.10",  "software none",
                                CV_DIAG_UNKNOWN_U32};
  sectorBlank();
  entryWrite(0u, &kWatchdog);
  entryWrite(1u, &kRequest);
  entryWrite(2u, &kTorn);
  entryTear(2u);
  entryWrite(3u, &kAfter);
}

// CV_LOG_ENTRIES valid entries from slot 0 up, either typical or with every field at its longest.
static void sectorFilled(int longest) {
  sectorBlank();
  for (uint32_t i = 0; i < CV_LOG_ENTRIES; ++i) {
    const int request = i % 4u == 3u;
    const Sample typical = {100u + i,
                            request ? CvDiagKindUpdateRequest : CvDiagKindBoot,
                            request ? 3600u + i : CV_DIAG_UNKNOWN_U32,
                            0x01000000u,
                            i % 3u,
                            CvReasonNone,
                            0u,
                            0u,
                            0u,
                            "v2.00.10",
                            "software none",
                            CvOptionAlreadySet};
    // The fault text fills the 63 characters a NUL-padded 64-byte field holds.
    const Sample widest = {1000u + i,
                           CvDiagKindUpdateRequest,
                           86400u + i,
                           0x24030000u,
                           3u,
                           CvReasonImageUnreadable,
                           0x08141230u,
                           123456u,
                           123456u,
                           "v2.00.10-bench",
                           "bus pc=0x08021A4C lr=0x08020F31 cfsr=0x00008200 bfar=0xDEADBEE0",
                           CvOptionChangeError};
    entryWrite(i, longest ? &widest : &typical);
  }
}

static int writeDiag(const char* dir, const char* name, const CvDriveState* state) {
  cvDiagWrite(state);
  return writeFile(dir, name, cvDiagText, CV_DIAG_TEXT_BYTES);
}

// The sample box of DIAG.TXT: a read fault in bank 1 the bootloader recorded, a refused file of
// another family, one failed write and the release's ROM .bin copied by mistake.
static void sampleBlock(struct CvBootBlock* block) {
  static const char kSummary[] = "v2.00.10 up 1234 s software none";
  memset(block, 0, sizeof *block);
  block->blockVersion = CV_BOOT_BLOCK_VERSION;
  block->lastReason = CvReasonImageUnreadable;
  block->faultAddress = 0x08045000u;
  memcpy(block->appSummary, kSummary, sizeof kSummary - 1u);
  block->bootFaultPc = 0x08002F1Cu;
  block->bootFaultCfsr = 0x00008200u;
  block->bootFaultBfar = 0x08045000u;
  block->optionBytes = CvOptionAlreadySet;
}

// The two files whose log must not be read, so there is no sector to read.
static int writeUnreadable(const char* dir, struct CvBootBlock* block, CvDriveState* state) {
  block->lastReason = CvReasonMenuHeld;
  block->faultAddress = 0x08141230u;
  state->log = NULL;
  if (writeDiag(dir, "DIAG-unreadable.TXT", state) != 0) return 1;
  block->faultAddress = 0x08101230u;
  return writeDiag(dir, "DIAG-bank2.TXT", state);
}

// The longest log lines under the widest header: every register and count at full width, the
// longest names, a full summary. 16 lines must still fit in DIAG.TXT.
static int writeCrowded(const char* dir, const CvDriveState* sample) {
  struct CvBootBlock block;
  memset(&block, 0, sizeof block);
  block.blockVersion = CV_BOOT_BLOCK_VERSION;
  block.lastReason = CvReasonImageUnreadable;
  block.faultAddress = 0xFFFFFFFFu;
  memset(block.appSummary, 'w', sizeof block.appSummary);
  block.bootFaultPc = 0xFFFFFFFFu;
  block.bootFaultCfsr = 0xFFFFFFFFu;
  block.bootFaultHfsr = 0xFFFFFFFFu;
  block.bootFaultBfar = 0xFFFFFFFFu;
  block.optionBytes = CvOptionChangeError;
  const struct CvImageReport image = {CvImageGood, "v2.00.10-bench"};
  const struct CvFaultRecord fault = {CV_FAULT_MAGIC, CvFaultKindMemManage, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                      0xFFFFFFFFu,    0xFFFFFFFFu,          0xFFFFFFFFu, CV_FLASH_SR_DBECCERR,
                                      0xFFFFFFFFu,    0u};
  CvDriveState state = *sample;
  state.block = &block;
  state.image = &image;
  state.fault = &fault;
  state.blocks = (CvBlockCounts){0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  state.clockStep = CvClockStepOscillator;
  sectorFilled(1);
  state.log = gSector;
  return writeDiag(dir, "DIAG-crowded.TXT", &state);
}

// A box whose crystal did not start and whose option bytes timed out at this power-on.
static int writeClock(const char* dir, struct CvBootBlock* block, CvDriveState* state) {
  sampleBlock(block);
  block->lastReason = CvReasonClockFailed;
  block->faultAddress = 0u;
  block->bootFaultPc = 0u;
  block->bootFaultCfsr = 0u;
  block->bootFaultBfar = 0u;
  block->optionBytes = CvOptionTimeout;
  state->clockStep = CvClockStepOscillator;
  state->optsrCur = CV_SAMPLE_OPTSR & ~(3u << 2);
  state->wpsnCur1 = 0xFFu;
  state->blocks = (CvBlockCounts){0u, 0u, 0u, 0u, 0u};
  state->wrongFile = "";
  sectorBlank();
  state->log = gSector;
  return writeDiag(dir, "DIAG-clock.TXT", state);
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "[ERROR] usage: drivetext OUT_DIR\n");
    return 1;
  }
  struct CvBootBlock block;
  sampleBlock(&block);
  const struct CvImageReport image = {CvImageGood, "v2.00.10"};
  const struct CvFaultRecord fault = {CV_FAULT_MAGIC, 2u,          0x08021A4Cu,          0x08020F31u,
                                      0x00008200u,    0x40000000u, 0xDEADBEE0u,          0xDEADBEE0u,
                                      CV_FLASH_SR_DBECCERR, 0x0123u, 0u};
  // 0x6DB66082 is the generic STM32H7 UF2 family, which the bootloader refuses.
  const CvBlockCounts blocks = {3u, 12u, 0x6DB66082u, 1u, 0x080A1200u};
  CvDriveState state = {&block,     &image,          &fault,         "0123456789ABCDEF01234567",
                        "0.21.0",   blocks,          "CUBEVO~1.BIN", 0u,
                        CV_SAMPLE_OPTSR, CV_SAMPLE_WPSN, gSector};

  if (writeFile(argv[1], "INFO_UF2.TXT", cvInfoText(&state), CV_INFO_TEXT_BYTES) != 0) return 1;

  sectorSample();
  if (writeDiag(argv[1], "DIAG.TXT", &state) != 0) return 1;
  sectorBlank();
  if (writeDiag(argv[1], "DIAG-empty.TXT", &state) != 0) return 1;
  sectorFilled(0);
  if (writeDiag(argv[1], "DIAG-full.TXT", &state) != 0) return 1;
  if (writeCrowded(argv[1], &state) != 0) return 1;
  if (writeUnreadable(argv[1], &block, &state) != 0) return 1;
  return writeClock(argv[1], &block, &state);
}
