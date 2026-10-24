// The update-mode screens. See screens.h.
//
// Every text keeps 1 px clear on all sides. The title is 9X12 when it fits the width and 5x7
// otherwise. Body lines are 5x7 on an 8 px pitch, word-wrapped to the 21 characters a line holds.
// The fonts are single case, so lowercase draws as capitals. A waiting screen keeps its bottom
// row for the status line.

#include "screens.h"

#include <string.h>

#include "boot_block.h"
#include "font.h"

#define CV_PAD        1  // clear pixels around every text
#define CV_LINE_PITCH 8  // a 5x7 line and the padding row it shares with the next
#define CV_BODY_LINES 4
#define CV_LINE_BYTES 48

typedef struct {
  const char* title;
  // Body strings in order, NULL after the last. Each may wrap onto several rows.
  const char* lines[CV_BODY_LINES];
  // The bottom row of a waiting screen, NULL when it has none.
  const char* status;
  bool waiting;
} ScreenText;

typedef struct {
  char text[CV_LINE_BYTES];
  uint32_t length;
} Line;

// Lines composed at draw time from boot_block.h and the installed image. resolve() matches them by
// address; distinct contents keep the three objects distinct.
static const char kInstalled[] = "<installed>";
static const char kCancel[] = "<cancel>";
static const char kStartFails[] = "<start fails>";

// The spec's table. "Writing..." stands for the spec's ellipsis: the fonts have no U+2026 glyph.
static const ScreenText kScreens[] = {
    [CvScreenWaitMenu] = {"UPDATE MODE", {"Drag cubevox.uf2", "onto the CUBEVOX drive.", kInstalled},
                          kCancel, true},
    [CvScreenWaitNoImage] = {"UPDATE NEEDED",
                             {"Last update did not", "finish.", "Drag cubevox.uf2", "onto the CUBEVOX drive."},
                             NULL, true},
    [CvScreenWaitBootFailed] = {"UPDATE NEEDED",
                                {"Firmware failed to", kStartFails, "Drag cubevox.uf2", "onto the CUBEVOX drive."},
                                NULL, true},
    [CvScreenWriting] = {"UPDATING", {"Writing...", "Do not unplug."}, NULL, false},
    [CvScreenWrongFile] = {"WRONG FILE", {"That is not a cubevox", "firmware file.", "Drag cubevox.uf2."},
                           NULL, false},
    [CvScreenDone] = {"DONE", {"Restarting."}, NULL, false},
};

static void lineAdd(Line* line, const char* text) {
  while (*text != '\0' && line->length + 1u < sizeof line->text) line->text[line->length++] = *text++;
  line->text[line->length] = '\0';
}

static void lineAddNumber(Line* line, uint32_t value) {
  char digits[11];
  uint32_t n = sizeof digits - 1u;
  digits[n] = '\0';
  do {
    digits[--n] = (char)('0' + value % 10u);
    value /= 10u;
  } while (value != 0u && n > 0u);
  lineAdd(line, &digits[n]);
}

// The text of a table line, composing the three run-time lines into `work`.
static const char* resolve(const char* line, const char* installed, Line* work) {
  work->length = 0u;
  work->text[0] = '\0';
  if (line == kInstalled) {
    lineAdd(work, "Installed ");
    lineAdd(work, installed);
  } else if (line == kCancel) {
    lineAdd(work, "Wait ");
    lineAddNumber(work, CV_UPDATE_TIMEOUT_MS / 1000u);
    lineAdd(work, " s to cancel.");
  } else if (line == kStartFails) {
    lineAdd(work, "start ");
    lineAddNumber(work, CV_BOOT_FAIL_LIMIT);
    lineAdd(work, " times.");
  } else {
    return line;
  }
  return work->text;
}

// ORs 8 vertical pixels, the top one at row y, into the one or two pages they cover.
static void orColumn(uint8_t* frame, int x, int y, uint8_t bits) {
  if (x < 0 || x >= CV_FRAME_WIDTH || y < 0 || bits == 0u) return;
  const int page = y / 8;
  const int shift = y % 8;
  if (page < CV_FRAME_HEIGHT / 8) frame[page * CV_FRAME_WIDTH + x] |= (uint8_t)(bits << shift);
  if (shift != 0 && page + 1 < CV_FRAME_HEIGHT / 8) {
    frame[(page + 1) * CV_FRAME_WIDTH + x] |= (uint8_t)(bits >> (8 - shift));
  }
}

// The glyph for `c`, a lowercase letter as its capital. NULL when the font has no such code.
static const uint8_t* glyphOf(const struct CvFont* font, char c) {
  uint8_t code = (uint8_t)c;
  if (code >= 'a' && code <= 'z') code = (uint8_t)(code - 'a' + 'A');
  int lo = 0;
  int hi = (int)font->glyphCount - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (font->codes[mid] == code) return font->glyphs + mid * font->bytesPerGlyph;
    if (font->codes[mid] < code) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return NULL;
}

static void drawGlyph(uint8_t* frame, int x, int y, const struct CvFont* font, const uint8_t* glyph) {
  const int bands = (font->height + 7) / 8;
  for (int band = 0; band < bands; ++band) {
    const int rows = font->height - band * 8;
    const uint8_t keep = rows >= 8 ? 0xFFu : (uint8_t)((1u << rows) - 1u);
    for (int col = 0; col < font->width; ++col) {
      orColumn(frame, x + col, y + band * 8, (uint8_t)(glyph[band * font->width + col] & keep));
    }
  }
}

static int textWidth(const struct CvFont* font, uint32_t length) {
  return length == 0u ? 0 : (int)length * (font->width + font->spacing) - font->spacing;
}

static void drawText(uint8_t* frame, int x, int y, const struct CvFont* font, const char* text, uint32_t length) {
  for (uint32_t i = 0; i < length; ++i) {
    const uint8_t* glyph = glyphOf(font, text[i]);
    if (glyph != NULL) drawGlyph(frame, x, y, font, glyph);
    x += font->width + font->spacing;
  }
}

// Length of the first line of `text` in `capacity` characters, broken at the last space that fits.
// `next` gets the start of the rest.
static uint32_t wrap(const char* text, uint32_t capacity, const char** next) {
  const uint32_t length = (uint32_t)strlen(text);
  if (length <= capacity) {
    *next = text + length;
    return length;
  }
  uint32_t cut = capacity;
  while (cut > 0u && text[cut] != ' ') --cut;
  if (cut == 0u) cut = capacity;
  *next = text[cut] == ' ' ? text + cut + 1 : text + cut;
  return cut;
}

// Draws `text` in 5x7 from body row `row`, wrapping, and stops after `lastRow`. Returns the next row.
static int drawLines(uint8_t* frame, int top, int row, int lastRow, const char* text) {
  const int advance = cvFont5x7.width + cvFont5x7.spacing;
  const uint32_t capacity = (uint32_t)((CV_FRAME_WIDTH - 2 * CV_PAD + cvFont5x7.spacing) / advance);
  while (*text != '\0' && row <= lastRow) {
    const char* next = text;
    const uint32_t length = wrap(text, capacity, &next);
    drawText(frame, CV_PAD, top + row * CV_LINE_PITCH, &cvFont5x7, text, length);
    text = next;
    ++row;
  }
  return row;
}

// The title in 9X12 when it fits the width, else in 5x7. Returns the y of the first body row.
static int drawTitle(uint8_t* frame, const char* title) {
  const uint32_t length = (uint32_t)strlen(title);
  const bool large = textWidth(&cvFont9x12, length) <= CV_FRAME_WIDTH - 2 * CV_PAD;
  const struct CvFont* font = large ? &cvFont9x12 : &cvFont5x7;
  drawText(frame, CV_PAD, CV_PAD, font, title, length);
  return CV_PAD + font->height + CV_PAD;
}

void cvScreenDraw(uint8_t frame[CV_FRAME_BYTES], CvScreen screen, const char* installed, bool connected) {
  memset(frame, 0, CV_FRAME_BYTES);
  if ((uint32_t)screen >= sizeof kScreens / sizeof kScreens[0]) return;
  const ScreenText* text = &kScreens[screen];
  const int top = drawTitle(frame, text->title);
  const int lastRow = (CV_FRAME_HEIGHT - CV_PAD - cvFont5x7.height - top) / CV_LINE_PITCH;
  const int bodyLastRow = text->waiting ? lastRow - 1 : lastRow;
  Line work;
  int row = 0;
  for (int i = 0; i < CV_BODY_LINES && text->lines[i] != NULL; ++i) {
    row = drawLines(frame, top, row, bodyLastRow, resolve(text->lines[i], installed ? installed : "", &work));
  }
  if (!text->waiting) return;
  const char* status = text->status ? resolve(text->status, "", &work) : NULL;
  if (connected) status = "Connected.";
  if (status != NULL) drawLines(frame, top, lastRow, lastRow, status);
}
