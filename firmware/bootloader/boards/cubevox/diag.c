// INFO_UF2.TXT and DIAG.TXT. See diag.h. Hex numbers print as the firmware's faultReport prints
// them: 0x, uppercase, no leading zeros.

#include "diag.h"

#include <string.h>

#include "board.h"

#define CV_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CV_INFO_TEXT_BYTES 640u
#define CV_FAULT_KIND_CLOCK 4u

// Entries DIAG.TXT lists, newest first.
#define CV_LOG_LINES 16u
#define CV_LOG_SLOTS (CV_DIAG_LOG_SIZE / CV_DIAG_ENTRY_SIZE)
// Kept free after the entries for the longest counts lines.
#define CV_LOG_COUNTS_BYTES 64u

// fault.cpp's kKindName and kClockStepName.
static const char* const kKindName[] = {"hard", "memmanage", "bus", "usage", "clock"};
static const char* const kClockStepName[] = {"?", "supply", "voltage scaling", "oscillator and PLL1",
                                             "bus clocks", "peripheral clocks"};

// Indexed by enum CvBootReason; the firmware's updateReasonName() uses the same names.
static const char* const kReasonName[] = {"none", "MenuHeld", "AppRequest", "ImageInvalid", "ImageUnreadable",
                                          "BootFailed"};
CV_STATIC_ASSERT(reason_names_cover_reasons, CV_ARRAY_SIZE(kReasonName) == (size_t)CvReasonBootFailed + 1u);

// Indexed by enum CvDiagKind; 0 and anything past the table print as "?".
static const char* const kDiagKindName[] = {"?", "boot", "update"};
CV_STATIC_ASSERT(kind_names_cover_kinds, CV_ARRAY_SIZE(kDiagKindName) == (size_t)CvDiagKindUpdateRequest + 1u);

char cvDiagText[CV_DIAG_TEXT_BYTES];

typedef struct {
  char* buffer;
  // Bytes, the NUL included.
  uint32_t capacity;
  uint32_t length;
  // A character did not fit.
  bool cut;
} Text;

static void put(Text* text, const char* s) {
  for (; *s != '\0'; ++s) {
    if (text->length + 1u >= text->capacity) {
      text->cut = true;
      break;
    }
    text->buffer[text->length++] = *s;
  }
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
  Text text = {info, sizeof info, 0u, false};
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

// Copies a NUL padded text field of `size` bytes to out[size + 1], a non-printable byte as '?'.
// Returns the length.
static uint32_t printable(char* out, const char* field, uint32_t size) {
  uint32_t n = 0;
  for (; n < size && field[n] != '\0'; ++n) out[n] = (field[n] >= 0x20 && field[n] < 0x7F) ? field[n] : '?';
  out[n] = '\0';
  return n;
}

// The application's summary as written.
static void putSummary(Text* text, const struct CvBootBlock* block) {
  char summary[sizeof block->appSummary + 1u];
  const uint32_t n = printable(summary, block->appSummary, sizeof block->appSummary);
  putLine(text, "app summary: ", n == 0u ? "none" : summary);
}

static const char* reasonText(uint32_t reason) {
  return reason < CV_ARRAY_SIZE(kReasonName) ? kReasonName[reason] : "unknown";
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

// ---- Flash log ---------------------------------------------------------------------------------

typedef enum { SlotBlank, SlotValid, SlotTorn } SlotState;

typedef struct {
  uint32_t valid;
  uint32_t torn;
  uint32_t blank;
  // Lines written so far.
  uint32_t listed;
  // A line did not fit, so older entries stay out too.
  bool full;
} LogScan;

static uint32_t readU32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// The contract in boot_block.h: blank is all 0xFF, valid has the magic and a matching CRC-32 over the
// bytes before it, anything else is torn.
static SlotState slotState(const uint8_t* slot) {
  uint8_t bits = 0xFFu;
  for (uint32_t i = 0; i < CV_DIAG_ENTRY_SIZE; ++i) bits &= slot[i];
  if (bits == 0xFFu) return SlotBlank;
  if (readU32(slot) != CV_DIAG_MAGIC) return SlotTorn;
  const uint32_t crc = cvCrc32(0u, slot, CV_DIAG_ENTRY_SIZE - 4u);
  return crc == readU32(slot + CV_DIAG_ENTRY_SIZE - 4u) ? SlotValid : SlotTorn;
}

// " label=" and the number in hex or decimal, or "?" when nobody knew it. A known value takes `unit`.
static void putMeasure(Text* text, const char* label, uint32_t value, bool hex, const char* unit) {
  put(text, label);
  if (value == CV_DIAG_UNKNOWN_U32) {
    put(text, "?");
    return;
  }
  if (hex) {
    putHex(text, value);
  } else {
    putDecimal(text, value);
  }
  put(text, unit);
}

// Writes one entry as `#sequence kind firmware up= rst= fails= reason fa= fault sai= ovr=`.
static void putEntry(Text* text, const uint8_t* slot) {
  struct CvDiagEntry e;
  char firmware[sizeof e.firmware + 1u];
  char fault[sizeof e.fault + 1u];
  memcpy(&e, slot, sizeof e);
  put(text, "#");
  putDecimal(text, e.sequence);
  put(text, " ");
  put(text, e.kind < CV_ARRAY_SIZE(kDiagKindName) ? kDiagKindName[e.kind] : "?");
  put(text, " ");
  put(text, printable(firmware, e.firmware, sizeof e.firmware) != 0u ? firmware : "?");
  putMeasure(text, " up=", e.uptimeS, false, "s");
  putMeasure(text, " rst=", e.resetCause, true, "");
  putMeasure(text, " fails=", e.bootFails, false, "");
  put(text, " ");
  put(text, reasonText(e.lastReason));
  putMeasure(text, " fa=", e.faultAddress, true, "");
  put(text, " ");
  put(text, printable(fault, e.fault, sizeof e.fault) != 0u ? fault : "-");
  putMeasure(text, " sai=", e.saiErrors, false, "");
  putMeasure(text, " ovr=", e.overruns, false, "");
  put(text, "\r\n");
}

// A line for a valid entry while lines remain. One that does not fit whole is dropped.
static void putListed(Text* text, LogScan* scan, const uint8_t* slot) {
  if (scan->listed >= CV_LOG_LINES || scan->full) return;
  const uint32_t mark = text->length;
  putEntry(text, slot);
  if (!text->cut) {
    scan->listed += 1u;
    return;
  }
  text->length = mark;
  text->buffer[mark] = '\0';
  text->cut = false;
  scan->full = true;
}

static void scanSlot(Text* text, LogScan* scan, const uint8_t* slot) {
  const SlotState state = slotState(slot);
  if (state == SlotBlank) {
    scan->blank += 1u;
  } else if (state == SlotTorn) {
    scan->torn += 1u;
  } else {
    scan->valid += 1u;
    putListed(text, scan, slot);
  }
}

// The closing lines. A log without a valid entry is empty, and the counts follow only when a slot is
// torn, so a log of nothing but torn slots still says so.
static void putCounts(Text* text, const LogScan* scan) {
  if (scan->valid == 0u) put(text, "flash log: empty\r\n");
  if (scan->valid == 0u && scan->torn == 0u) return;
  put(text, "flash log: ");
  putDecimal(text, scan->valid);
  put(text, " valid, ");
  putDecimal(text, scan->torn);
  put(text, " torn, ");
  putDecimal(text, scan->blank);
  put(text, " blank\r\n");
}

// The application only appends and erases the whole sector, so slot order is sequence order and the
// highest slot is the newest entry.
uint32_t cvDiagLogLines(const struct CvBootBlock* block, const uint8_t* sector, char* out, uint32_t capacity) {
  Text text = {out, capacity, 0u, false};
  const uint32_t at = block->faultAddress;
  if (at >= CV_DIAG_LOG_ADDR && at - CV_DIAG_LOG_ADDR < CV_DIAG_LOG_SIZE) {
    put(&text, "flash log: unreadable at ");
    putHex(&text, at);
    put(&text, "\r\n");
    return text.length;
  }
  LogScan scan = {0u, 0u, 0u, 0u, false};
  text.capacity = capacity > CV_LOG_COUNTS_BYTES ? capacity - CV_LOG_COUNTS_BYTES : 1u;
  for (uint32_t slot = CV_LOG_SLOTS; slot > 0u; --slot) {
    scanSlot(&text, &scan, sector + (slot - 1u) * CV_DIAG_ENTRY_SIZE);
  }
  text.capacity = capacity;
  putCounts(&text, &scan);
  return text.length;
}

void cvDiagWrite(const CvDriveState* state) {
  // Room for the closing CR LF after the padding.
  Text text = {cvDiagText, CV_DIAG_TEXT_BYTES - 2u, 0u, false};
  put(&text, "cubevox bootloader v" CV_BOOT_VERSION "\r\n");
  putLine(&text, "board: ", UF2_BOARD_ID);
  putLine(&text, "serial: ", state->serial);
  putLine(&text, "reason: ", reasonText(state->block->lastReason));
  put(&text, "fault address: ");
  putHex(&text, state->block->faultAddress);
  put(&text, "\r\n");
  putSummary(&text, state->block);
  putFault(&text, state->fault);
  putRefused(&text, state);
  text.length += cvDiagLogLines(state->block, state->log, &text.buffer[text.length], text.capacity - text.length);
  for (uint32_t i = text.length; i < CV_DIAG_TEXT_BYTES - 2u; ++i) cvDiagText[i] = ' ';
  cvDiagText[CV_DIAG_TEXT_BYTES - 2u] = '\r';
  cvDiagText[CV_DIAG_TEXT_BYTES - 1u] = '\n';
}
