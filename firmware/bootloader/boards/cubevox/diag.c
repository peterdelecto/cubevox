// INFO_UF2.TXT and DIAG.TXT. See diag.h. Hex numbers print as the firmware's faultReport prints
// them: 0x, uppercase, no leading zeros.

#include "diag.h"

#include <string.h>

#include "board.h"
#include "boot_decide.h"
#include "text.h"

#define CV_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

// Entries DIAG.TXT lists, newest first.
#define CV_LOG_LINES 16u
#define CV_LOG_SLOTS (CV_DIAG_LOG_SIZE / CV_DIAG_ENTRY_SIZE)
// Kept free after the entries for the longest counts lines.
#define CV_LOG_COUNTS_BYTES 64u

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
  char digits[11] = "0x";
  int8_t shift = 28;
  while (shift > 0 && ((value >> shift) & 0xFu) == 0u) shift = (int8_t)(shift - 4);
  uint32_t n = 2u;
  for (; shift >= 0; shift = (int8_t)(shift - 4)) digits[n++] = cvTextHexDigit(value >> shift);
  digits[n] = '\0';
  put(text, digits);
}

static void putDecimal(Text* text, uint32_t value) {
  char digits[11];
  put(text, cvTextDecimal(digits, value));
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

// Spaces to the last two bytes, then CR LF, so a file of fixed size ends like every line.
static void padToSize(char* buffer, uint32_t length, uint32_t size) {
  for (uint32_t i = length; i < size - 2u; ++i) buffer[i] = ' ';
  buffer[size - 2u] = '\r';
  buffer[size - 1u] = '\n';
}

const char* cvInstalledText(const struct CvImageReport* image) {
  if (image->state == CvImageNone) return "none";
  if (image->state == CvImageBad) return "invalid";
  return image->version;
}

const char* cvInfoText(const CvDriveState* state) {
  static char info[CV_INFO_TEXT_BYTES];
  Text text = {info, sizeof info - 2u, 0u, false};
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
  padToSize(info, text.length, sizeof info);
  return info;
}

// The application's summary as written.
static void putSummary(Text* text, const struct CvBootBlock* block) {
  char summary[sizeof block->appSummary + 1u];
  const uint32_t n = cvTextPrintable(summary, block->appSummary, sizeof block->appSummary);
  putLine(text, "app summary: ", n == 0u ? "none" : summary);
}

// The registers of the bootloader's own last fault.
static void putBootFault(Text* text, const struct CvBootBlock* block) {
  put(text, "boot fault:");
  if ((block->bootFaultPc | block->bootFaultCfsr | block->bootFaultHfsr | block->bootFaultBfar) == 0u) {
    put(text, " none\r\n");
    return;
  }
  putField(text, " pc", block->bootFaultPc);
  putField(text, " cfsr", block->bootFaultCfsr);
  putField(text, " hfsr", block->bootFaultHfsr);
  putField(text, " bfar", block->bootFaultBfar);
  put(text, "\r\n");
}

static void putClock(Text* text, uint32_t clockStep) {
  if (clockStep == 0u) {
    put(text, "clock: crystal\r\n");
    return;
  }
  put(text, "clock: failed in ");
  put(text, cvClockStepName(clockStep));
  put(text, ", USB on the internal oscillator\r\n");
}

// The option byte step's result and the values in force now.
static void putOptions(Text* text, const CvDriveState* state) {
  put(text, "option bytes: ");
  put(text, cvOptionResultName(state->block->optionBytes));
  put(text, ", BOR level ");
  putDecimal(text, (state->optsrCur & CV_OPTSR_BOR_LEV_MASK) >> CV_OPTSR_BOR_LEV_POS);
  put(text, (state->wpsnCur1 & CV_WPSN_SECTOR0) == 0u ? ", sector 0 protected\r\n" : ", sector 0 writable\r\n");
}

// The fault record as faultReport prints it over serial.
static void putFault(Text* text, const struct CvFaultRecord* r) {
  put(text, "fault: ");
  if (r->magic != CV_FAULT_MAGIC) {
    put(text, "none\r\n");
    return;
  }
  if (r->kind == CvFaultKindClock) {
    put(text, "clock setup failed in ");
    put(text, cvClockStepName(r->detail));
    put(text, "\r\n");
    return;
  }
  put(text, cvFaultKindName(r->kind));
  putField(text, " pc", r->pc);
  putField(text, " lr", r->lr);
  putField(text, " cfsr", r->cfsr);
  putField(text, " hfsr", r->hfsr);
  putField(text, " bfar", r->bfar);
  putField(text, " mmfar", r->mmfar);
  if (r->sr2 & CV_FLASH_SR_DBECCERR) {
    putField(text, " bank2-ecc", CV_BANK2_BASE + (r->eccFa2 & CV_FLASH_ECC_ADDR) * CV_FLASH_WORD_BYTES);
  }
  put(text, "\r\n");
}

static void putBlocks(Text* text, const CvBlockCounts* blocks) {
  put(text, "blocks: ");
  putDecimal(text, blocks->written);
  put(text, " written, ");
  putDecimal(text, blocks->refused);
  put(text, " refused, ");
  putDecimal(text, blocks->failed);
  put(text, " failed");
  if (blocks->refused != 0u) {
    put(text, ", last family ");
    putHex(text, blocks->lastRefusedFamily);
  }
  if (blocks->failed != 0u) {
    put(text, ", first failure at ");
    putHex(text, blocks->firstFailedAddress);
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

// Writes one entry as `#sequence kind firmware up= rst= fails= reason fa= fault sai= ovr=`, and
// `ob=` when the option byte step failed at that boot.
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
  put(text, cvTextPrintable(firmware, e.firmware, sizeof e.firmware) != 0u ? firmware : "?");
  putMeasure(text, " up=", e.uptimeS, false, "s");
  putMeasure(text, " rst=", e.resetCause, true, "");
  putMeasure(text, " fails=", e.bootFails, false, "");
  put(text, " ");
  put(text, cvBootReasonName(e.lastReason));
  putMeasure(text, " fa=", e.faultAddress, true, "");
  put(text, " ");
  put(text, cvTextPrintable(fault, e.fault, sizeof e.fault) != 0u ? fault : "-");
  putMeasure(text, " sai=", e.saiErrors, false, "");
  putMeasure(text, " ovr=", e.overruns, false, "");
  if (cvOptionResultFailed(e.optionBytes)) {
    put(text, " ob=");
    put(text, cvOptionResultName(e.optionBytes));
  }
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
  if (at >= CV_BANK2_BASE && at < CV_BANK2_END) {
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
  putLine(&text, "installed: ", cvInstalledText(state->image));
  putLine(&text, "reason: ", cvBootReasonName(state->block->lastReason));
  put(&text, "fault address: ");
  putHex(&text, state->block->faultAddress);
  put(&text, "\r\n");
  putBootFault(&text, state->block);
  putClock(&text, state->clockStep);
  putOptions(&text, state);
  putSummary(&text, state->block);
  putFault(&text, state->fault);
  putBlocks(&text, &state->blocks);
  putLine(&text, "wrong file: ", state->wrongFile[0] != '\0' ? state->wrongFile : "none");
  text.length += cvDiagLogLines(state->block, state->log, &text.buffer[text.length], text.capacity - text.length);
  padToSize(cvDiagText, text.length, CV_DIAG_TEXT_BYTES);
}
