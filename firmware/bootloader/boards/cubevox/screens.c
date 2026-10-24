// The update-mode screens. See screens.h.
//
// Line i is drawn in 5x7 on row 2 * i of the panel's eight 8 px rows, so rows 1, 3, 5 and 7 stay
// blank. Every text keeps 1 px clear on all sides. The fonts are single case, so lowercase draws as
// capitals.

#include "screens.h"

#include <string.h>

#include "boot_block.h"
#include "font.h"

#define CV_PAD    1  // clear pixels around every text
#define CV_ROW_PX 8  // one of the panel's eight rows

typedef struct {
  // The four lines in order, NULL for a blank one.
  const char* lines[CV_SCREEN_LINES];
  // A waiting screen swaps its last line for "Computer connected." while a host is connected.
  bool waiting;
} ScreenText;

typedef struct {
  char* text;
  uint32_t size;
  uint32_t length;
} Line;

// Lines composed at run time from boot_block.h and the installed image. composeLine() matches them
// by address; distinct contents keep the two objects distinct.
static const char kInstalled[] = "<installed>";
static const char kCancel[] = "<cancel>";

// The spec's table, rows 0 / 2 / 4 / 6.
static const ScreenText kScreens[] = {
    [CvScreenWaitMenu] = {{"Drop the firmware on", "the CUBEVOX drive.", kInstalled, kCancel}, true},
    [CvScreenWaitNoImage] = {{"The update stopped.", "Drop the firmware on", "the CUBEVOX drive.", NULL}, true},
    [CvScreenWaitBootFailed] = {{"Startup failed.", "Drop the firmware on", "the CUBEVOX drive.", NULL}, true},
    [CvScreenWriting] = {{"Writing the firmware.", "Do not unplug.", NULL, NULL}, false},
    [CvScreenWrongFile] = {{"That is not a cubevox", "firmware file.", "Drop the firmware on",
                            "the CUBEVOX drive."},
                           false},
    [CvScreenDone] = {{"The update is done.", "Restarting now.", NULL, NULL}, false},
};

// Appends at most `limit` characters of `text`.
static void lineAddLimit(Line* line, const char* text, uint32_t limit) {
  for (uint32_t i = 0; i < limit && text[i] != '\0' && line->length + 1u < line->size; ++i) {
    line->text[line->length++] = text[i];
  }
  line->text[line->length] = '\0';
}

static void lineAdd(Line* line, const char* text) {
  lineAddLimit(line, text, line->size);
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

// Writes a table line: fixed wording as it stands, the two run-time lines composed. The release
// name is cut so the closing period stays inside the line.
static void composeLine(Line* line, const char* source, const char* installed) {
  if (source == kInstalled) {
    lineAdd(line, "You have ");
    lineAddLimit(line, installed, CV_SCREEN_LINE_CHARS - line->length - 1u);
    lineAdd(line, ".");
  } else if (source == kCancel) {
    lineAdd(line, "Wait ");
    lineAddNumber(line, CV_UPDATE_TIMEOUT_MS / 1000u);
    lineAdd(line, " s to cancel.");
  } else if (source != NULL) {
    lineAdd(line, source);
  }
}

void cvScreenCompose(CvScreen screen, const char* installed, bool connected, CvScreenText* out) {
  for (uint32_t i = 0; i < CV_SCREEN_LINES; ++i) out->line[i][0] = '\0';
  if ((uint32_t)screen >= sizeof kScreens / sizeof kScreens[0]) return;
  const ScreenText* text = &kScreens[screen];
  for (uint32_t i = 0; i < CV_SCREEN_LINES; ++i) {
    Line line = {out->line[i], sizeof out->line[i], 0u};
    const bool hostLine = text->waiting && connected && i == CV_SCREEN_LINES - 1u;
    composeLine(&line, hostLine ? "Computer connected." : text->lines[i], installed != NULL ? installed : "");
  }
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

static void drawText(uint8_t* frame, int x, int y, const struct CvFont* font, const char* text, uint32_t length) {
  for (uint32_t i = 0; i < length; ++i) {
    const uint8_t* glyph = glyphOf(font, text[i]);
    if (glyph != NULL) drawGlyph(frame, x, y, font, glyph);
    x += font->width + font->spacing;
  }
}

void cvScreenDraw(uint8_t frame[CV_FRAME_BYTES], CvScreen screen, const char* installed, bool connected) {
  CvScreenText text;
  memset(frame, 0, CV_FRAME_BYTES);
  cvScreenCompose(screen, installed, connected, &text);
  for (uint32_t i = 0; i < CV_SCREEN_LINES; ++i) {
    // The wording fits, and proof --check says so. The cut keeps a fault in it on the panel.
    uint32_t length = (uint32_t)strlen(text.line[i]);
    if (length > CV_SCREEN_LINE_CHARS) length = CV_SCREEN_LINE_CHARS;
    drawText(frame, CV_PAD, (int)(2u * i) * CV_ROW_PX + CV_PAD, &cvFont5x7, text.line[i], length);
  }
}
