// INFO_UF2.TXT and DIAG.TXT. See diag.h. Hex numbers print as the firmware's faultReport prints
// them: 0x, uppercase, no leading zeros.

#include "diag.h"

#include "board.h"

#define CV_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CV_INFO_TEXT_BYTES 640u
#define CV_FAULT_KIND_CLOCK 4u

// fault.cpp's kKindName and kClockStepName.
static const char* const kKindName[] = {"hard", "memmanage", "bus", "usage", "clock"};
static const char* const kClockStepName[] = {"?", "supply", "voltage scaling", "oscillator and PLL1",
                                             "bus clocks", "peripheral clocks"};

// Indexed by enum CvBootReason; the firmware's updateReasonName() uses the same names.
static const char* const kReasonName[] = {"none", "MenuHeld", "AppRequest", "ImageInvalid", "ImageUnreadable",
                                          "BootFailed"};
CV_STATIC_ASSERT(reason_names_cover_reasons, CV_ARRAY_SIZE(kReasonName) == (size_t)CvReasonBootFailed + 1u);

char cvDiagText[CV_DIAG_TEXT_BYTES];

typedef struct {
  char* buffer;
  // Bytes, the NUL included.
  uint32_t capacity;
  uint32_t length;
} Text;

static void put(Text* text, const char* s) {
  while (*s != '\0' && text->length + 1u < text->capacity) text->buffer[text->length++] = *s++;
  text->buffer[text->length] = '\0';
}

static void putHex(Text* text, uint32_t value) {
  static const char kHex[] = "0123456789ABCDEF";
  char digits[11] = "0x";
  int shift = 28;
  while (shift > 0 && ((value >> shift) & 0xFu) == 0u) shift -= 4;
  uint32_t n = 2u;
  for (; shift >= 0; shift -= 4) digits[n++] = kHex[(value >> shift) & 0xFu];
  digits[n] = '\0';
  put(text, digits);
}

static void putDecimal(Text* text, uint32_t value) {
  char digits[11];
  uint32_t n = sizeof digits - 1u;
  digits[n] = '\0';
  do {
    digits[--n] = (char)('0' + value % 10u);
    value /= 10u;
  } while (value != 0u && n > 0u);
  put(text, &digits[n]);
}

static void putLine(Text* text, const char* label, const char* value) {
  put(text, label);
  put(text, value);
  put(text, "\r\n");
}

static void putField(Text* text, const char* label, uint32_t value) {
  put(text, label);
  put(text, "=");
  putHex(text, value);
}

const char* cvInstalledText(const struct CvImageReport* image) {
  if (image->state == CvImageNone) return "none";
  if (image->state == CvImageBad) return "invalid";
  return image->version;
}

const char* cvInfoText(const CvDriveState* state, uint32_t* length) {
  static char info[CV_INFO_TEXT_BYTES];
  Text text = {info, sizeof info, 0u};
  put(&text, "cubevox bootloader v" CV_BOOT_VERSION "\r\n");
  put(&text, "Model: " UF2_PRODUCT_NAME "\r\n");
  put(&text, "Board-ID: " UF2_BOARD_ID "\r\n");
  putLine(&text, "Serial: ", state->serial);
  putLine(&text, "Installed: ", cvInstalledText(state->image));
  put(&text, "TinyUF2 " CV_TINYUF2_VERSION ", MIT License, Copyright (c) 2020 Ha Thach\r\n");
  put(&text, "TinyUSB ");
  put(&text, state->tinyusbVersion);
  put(&text, ", MIT License, Copyright (c) 2012-2026, Ha Thach (tinyusb.org)\r\n");
  put(&text, "Licence texts: firmware/bootloader/README.md at github.com/peterdelecto/cubevox\r\n");
  *length = text.length;
  return info;
}

// The application's summary as written, a non-printable byte as '?'.
static void putSummary(Text* text, const struct CvBootBlock* block) {
  char summary[sizeof block->appSummary + 1u];
  uint32_t n = 0;
  for (; n < sizeof block->appSummary && block->appSummary[n] != '\0'; ++n) {
    const char c = block->appSummary[n];
    summary[n] = (c >= 0x20 && c < 0x7F) ? c : '?';
  }
  summary[n] = '\0';
  putLine(text, "app summary: ", n == 0u ? "none" : summary);
}

// The fault record as faultReport prints it over serial.
static void putFault(Text* text, const struct CvFaultRecord* r) {
  put(text, "fault: ");
  if (r->magic != CV_FAULT_MAGIC) {
    put(text, "none\r\n");
    return;
  }
  if (r->kind == CV_FAULT_KIND_CLOCK) {
    put(text, "clock setup failed in ");
    put(text, kClockStepName[r->detail < CV_ARRAY_SIZE(kClockStepName) ? r->detail : 0u]);
    put(text, "\r\n");
    return;
  }
  put(text, r->kind < CV_ARRAY_SIZE(kKindName) ? kKindName[r->kind] : "unknown");
  putField(text, " pc", r->pc);
  putField(text, " lr", r->lr);
  putField(text, " cfsr", r->cfsr);
  putField(text, " hfsr", r->hfsr);
  putField(text, " bfar", r->bfar);
  putField(text, " mmfar", r->mmfar);
  if (r->sr2 & CV_FLASH_SR_DBECCERR) {
    putField(text, " bank2-ecc", CV_FLASH_BANK2_BASE + (r->eccFa2 & CV_FLASH_ECC_ADDR) * CV_FLASH_WORD_BYTES);
  }
  put(text, "\r\n");
}

static void putRefused(Text* text, const CvDriveState* state) {
  put(text, "refused blocks: ");
  putDecimal(text, state->refusedBlocks);
  if (state->refusedBlocks != 0u) {
    put(text, ", last family ");
    putHex(text, state->lastRefusedFamily);
  }
  put(text, "\r\n");
}

void cvDiagWrite(const CvDriveState* state) {
  // Room for the closing CR LF after the padding.
  Text text = {cvDiagText, CV_DIAG_TEXT_BYTES - 2u, 0u};
  const uint32_t reason = state->block->lastReason;
  put(&text, "cubevox bootloader v" CV_BOOT_VERSION "\r\n");
  putLine(&text, "board: ", UF2_BOARD_ID);
  putLine(&text, "serial: ", state->serial);
  putLine(&text, "reason: ", reason < CV_ARRAY_SIZE(kReasonName) ? kReasonName[reason] : "unknown");
  put(&text, "fault address: ");
  putHex(&text, state->block->faultAddress);
  put(&text, "\r\n");
  putSummary(&text, state->block);
  putFault(&text, state->fault);
  putRefused(&text, state);
  put(&text, "flash log: not in this build\r\n");
  for (uint32_t i = text.length; i < CV_DIAG_TEXT_BYTES - 2u; ++i) cvDiagText[i] = ' ';
  cvDiagText[CV_DIAG_TEXT_BYTES - 2u] = '\r';
  cvDiagText[CV_DIAG_TEXT_BYTES - 1u] = '\n';
}
