// Draws every update-mode screen with screens.c and writes OUT_DIR/NN-<state>.png at four times the
// panel's size, white on black like the panel, inside a grey margin that marks the panel's edge.
// The PNG holds stored deflate blocks, so no library is needed. build.sh --proofs runs it.
//
// With --check it writes nothing and asserts the approved wording and the owner's rules on every
// screen. Each line is the approved text, at most 21 characters and never names the file extension.
// The drawn frame carries text on rows 0, 2, 4 and 6 only, with 1 px clear on all sides.
// build.sh --selftest runs it.
//
//   proof OUT_DIR
//   proof --check

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "image_check.h"
#include "screens.h"

#define CV_SCALE      4
#define CV_MARGIN     8
#define CV_PNG_WIDTH  (CV_FRAME_WIDTH * CV_SCALE + 2 * CV_MARGIN)
#define CV_PNG_HEIGHT (CV_FRAME_HEIGHT * CV_SCALE + 2 * CV_MARGIN)
#define CV_ROW_BYTES  (1 + CV_PNG_WIDTH)
#define CV_RAW_BYTES  (CV_ROW_BYTES * CV_PNG_HEIGHT)
#define CV_STORED_MAX 65535u
#define CV_ZLIB_BYTES (2 + CV_RAW_BYTES + 5 * (CV_RAW_BYTES / CV_STORED_MAX + 1) + 4)

#define CV_GREY_MARGIN 0x40u
#define CV_PIXEL_ON    0xFFu

typedef struct {
  const char* name;
  CvScreen screen;
  bool connected;
} Proof;

// One proof per state of the spec's OLED table; 04 is the menu screen with a host connected.
static const Proof kProofs[] = {
    {"wait-menu", CvScreenWaitMenu, false},      {"wait-no-image", CvScreenWaitNoImage, false},
    {"wait-boot-failed", CvScreenWaitBootFailed, false}, {"host-connected", CvScreenWaitMenu, true},
    {"writing", CvScreenWriting, false},         {"wrong-file", CvScreenWrongFile, false},
    {"done", CvScreenDone, false},               {"cable-out", CvScreenCableOut, false},
    {"clock-failed", CvScreenClockFailed, false},
};

// The release the menu proofs show as installed.
static const char kInstalledExample[] = "v2.00.10";

static uint8_t gRaw[CV_RAW_BYTES];
static uint8_t gZlib[CV_ZLIB_BYTES];

static void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static bool pixelOn(const uint8_t* frame, int x, int y) {
  return (frame[(y / 8) * CV_FRAME_WIDTH + x] >> (y % 8)) & 1u;
}

// Grey margin, then the panel scaled up; each row starts with filter byte 0.
static void rasterize(const uint8_t* frame) {
  for (int y = 0; y < CV_PNG_HEIGHT; ++y) {
    uint8_t* row = &gRaw[y * CV_ROW_BYTES];
    row[0] = 0u;
    for (int x = 0; x < CV_PNG_WIDTH; ++x) {
      const int px = (x - CV_MARGIN) / CV_SCALE;
      const int py = (y - CV_MARGIN) / CV_SCALE;
      const bool inPanel = x >= CV_MARGIN && y >= CV_MARGIN && px < CV_FRAME_WIDTH && py < CV_FRAME_HEIGHT;
      row[1 + x] = !inPanel ? CV_GREY_MARGIN : (pixelOn(frame, px, py) ? CV_PIXEL_ON : 0u);
    }
  }
}

static uint32_t adler32(const uint8_t* data, uint32_t length) {
  uint32_t a = 1u;
  uint32_t b = 0u;
  for (uint32_t i = 0; i < length; ++i) {
    a = (a + data[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

// A zlib stream of stored deflate blocks. Returns its length.
static uint32_t zlibStore(const uint8_t* raw, uint32_t length, uint8_t* out) {
  uint32_t n = 0;
  out[n++] = 0x78u;
  out[n++] = 0x01u;
  for (uint32_t done = 0; done < length;) {
    const uint32_t chunk = length - done < CV_STORED_MAX ? length - done : CV_STORED_MAX;
    out[n++] = done + chunk == length ? 1u : 0u;
    out[n++] = (uint8_t)chunk;
    out[n++] = (uint8_t)(chunk >> 8);
    out[n++] = (uint8_t)~chunk;
    out[n++] = (uint8_t)(~chunk >> 8);
    memcpy(out + n, raw + done, chunk);
    n += chunk;
    done += chunk;
  }
  put32(out + n, adler32(raw, length));
  return n + 4u;
}

static bool writeChunk(FILE* file, const char* type, const uint8_t* data, uint32_t length) {
  uint8_t head[8];
  uint8_t tail[4];
  put32(head, length);
  memcpy(head + 4, type, 4);
  put32(tail, cvCrc32(cvCrc32(0u, (const uint8_t*)type, 4u), data, length));
  return fwrite(head, 1, sizeof head, file) == sizeof head &&
         (length == 0u || fwrite(data, 1, length, file) == length) && fwrite(tail, 1, sizeof tail, file) == sizeof tail;
}

// 8-bit greyscale PNG of the frame.
static bool pngWrite(const char* path, const uint8_t* frame) {
  static const uint8_t kSignature[8] = {0x89u, 'P', 'N', 'G', '\r', '\n', 0x1Au, '\n'};
  uint8_t header[13] = {0};
  put32(header, CV_PNG_WIDTH);
  put32(header + 4, CV_PNG_HEIGHT);
  header[8] = 8u;
  rasterize(frame);
  const uint32_t zlibBytes = zlibStore(gRaw, CV_RAW_BYTES, gZlib);
  FILE* file = fopen(path, "wb");
  if (file == NULL) return false;
  bool ok = fwrite(kSignature, 1, sizeof kSignature, file) == sizeof kSignature;
  ok = ok && writeChunk(file, "IHDR", header, sizeof header);
  ok = ok && writeChunk(file, "IDAT", gZlib, zlibBytes);
  ok = ok && writeChunk(file, "IEND", NULL, 0u);
  return fclose(file) == 0 && ok;
}

// The approved wording, rows 0 / 2 / 4 / 6 of each screen as the spec table gives it, "" for a blank
// line. The composed text must equal it, so a change to screens.c the owner has not approved fails
// the selftest. Entries named for a connected host show that a waiting screen swaps its last line
// for "Computer connected." and every other screen keeps its own.
typedef struct {
  const char* name;
  CvScreen screen;
  bool connected;
  const char* lines[CV_SCREEN_LINES];
} Wording;

static const Wording kWording[] = {
    {"wait-menu", CvScreenWaitMenu, false,
     {"Drop the firmware on", "the CUBEVOX drive.", "You have v2.00.10.", "Wait 60 s to cancel."}},
    {"wait-no-image", CvScreenWaitNoImage, false,
     {"The update stopped.", "Drop the firmware on", "the CUBEVOX drive.", ""}},
    {"wait-boot-failed", CvScreenWaitBootFailed, false,
     {"Startup failed.", "Drop the firmware on", "the CUBEVOX drive.", ""}},
    {"writing", CvScreenWriting, false, {"Writing the firmware.", "Do not unplug.", "", ""}},
    {"wrong-file", CvScreenWrongFile, false,
     {"That is not a cubevox", "firmware file.", "Drop the firmware on", "the CUBEVOX drive."}},
    {"done", CvScreenDone, false, {"The update is done.", "Restarting now.", "", ""}},
    {"cable-out", CvScreenCableOut, false,
     {"The cable came out.", "Plug it back in and", "drop the firmware on", "the CUBEVOX drive."}},
    {"clock-failed", CvScreenClockFailed, false,
     {"The clock failed.", "Send the DIAG.TXT", "file on the CUBEVOX", "drive for a repair."}},
    {"cable-out-connected", CvScreenCableOut, true,
     {"The cable came out.", "Plug it back in and", "drop the firmware on", "the CUBEVOX drive."}},
    {"host-connected", CvScreenWaitMenu, true,
     {"Drop the firmware on", "the CUBEVOX drive.", "You have v2.00.10.", "Computer connected."}},
    {"no-image-connected", CvScreenWaitNoImage, true,
     {"The update stopped.", "Drop the firmware on", "the CUBEVOX drive.", "Computer connected."}},
    {"boot-failed-connected", CvScreenWaitBootFailed, true,
     {"Startup failed.", "Drop the firmware on", "the CUBEVOX drive.", "Computer connected."}},
};

// Reports a broken rule and returns 1, so the checks combine with |.
static int broken(const char* screen, const char* what, const char* rule) {
  fprintf(stderr, "[ERROR] screen %s: '%s' %s\n", screen, what, rule);
  return 1;
}

static bool hasText(const char* line) {
  for (; *line != '\0'; ++line) {
    if (*line != ' ') return true;
  }
  return false;
}

// True when the line holds "uf2" in any case.
static bool namesExtension(const char* line) {
  for (; *line != '\0'; ++line) {
    if (tolower((unsigned char)line[0]) == 'u' && tolower((unsigned char)line[1]) == 'f' && line[2] == '2') return true;
  }
  return false;
}

static int checkLines(const Wording* wording, const CvScreenText* text) {
  int bad = 0;
  for (uint32_t i = 0; i < CV_SCREEN_LINES; ++i) {
    const char* line = text->line[i];
    if (strcmp(line, wording->lines[i]) != 0) bad |= broken(wording->name, line, "differs from the approved wording");
    if (strlen(line) > CV_SCREEN_LINE_CHARS) bad |= broken(wording->name, line, "is over 21 characters");
    if (namesExtension(line)) bad |= broken(wording->name, line, "names the file extension");
  }
  return bad;
}

// Whether any pixel of the 8 px row `page` is lit.
static bool rowLit(const uint8_t* frame, int page) {
  for (int x = 0; x < CV_FRAME_WIDTH; ++x) {
    if (frame[page * CV_FRAME_WIDTH + x] != 0u) return true;
  }
  return false;
}

// The outermost ring of pixels, which every text must leave clear.
static bool ringLit(const uint8_t* frame) {
  const int last = CV_FRAME_HEIGHT / 8 - 1;
  for (int page = 0; page <= last; ++page) {
    if (frame[page * CV_FRAME_WIDTH] != 0u || frame[page * CV_FRAME_WIDTH + CV_FRAME_WIDTH - 1] != 0u) return true;
  }
  for (int x = 0; x < CV_FRAME_WIDTH; ++x) {
    if ((frame[x] & 0x01u) != 0u || (frame[last * CV_FRAME_WIDTH + x] & 0x80u) != 0u) return true;
  }
  return false;
}

// Row 2 * i is lit exactly when line i has text, rows 1, 3, 5 and 7 stay dark, the ring stays dark.
static int checkFrame(const char* screen, const CvScreenText* text, const uint8_t* frame) {
  int bad = 0;
  for (int page = 0; page < CV_FRAME_HEIGHT / 8; ++page) {
    const bool wantLit = page % 2 == 0 && hasText(text->line[page / 2]);
    if (rowLit(frame, page) == wantLit) continue;
    const char* rows[] = {"row 0", "row 1", "row 2", "row 3", "row 4", "row 5", "row 6", "row 7"};
    bad |= broken(screen, rows[page], wantLit ? "has a line but draws nothing" : "must stay blank but draws");
  }
  if (ringLit(frame)) bad |= broken(screen, "frame edge", "is lit, so a text lacks its 1 px of clearance");
  return bad;
}

static int checkWording(const Wording* wording) {
  CvScreenText text;
  uint8_t frame[CV_FRAME_BYTES];
  cvScreenCompose(wording->screen, kInstalledExample, wording->connected, &text);
  cvScreenDraw(frame, wording->screen, kInstalledExample, wording->connected);
  return checkLines(wording, &text) | checkFrame(wording->name, &text, frame);
}

// A release name longer than the line leaves room for is cut, and the period stays.
static int checkLongRelease(void) {
  static const char kLongRelease[] = "v2.00.10-bench99";
  CvScreenText text;
  uint8_t frame[CV_FRAME_BYTES];
  cvScreenCompose(CvScreenWaitMenu, kLongRelease, false, &text);
  cvScreenDraw(frame, CvScreenWaitMenu, kLongRelease, false);
  int bad = checkFrame("long-release", &text, frame);
  if (strcmp(text.line[2], "You have v2.00.10-be.") != 0) {
    bad |= broken("long-release", text.line[2], "is not the cut release name with its period");
  }
  return bad;
}

static int check(void) {
  int bad = checkLongRelease();
  for (uint32_t i = 0; i < sizeof kWording / sizeof kWording[0]; ++i) bad |= checkWording(&kWording[i]);
  if (bad == 0) {
    printf("[check] PASS: %u screens use the approved wording on rows 0, 2, 4 and 6 only, in 21 characters\n",
           (unsigned)(sizeof kWording / sizeof kWording[0]));
  }
  return bad;
}

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "--check") == 0) return check();
  if (argc != 2) {
    fprintf(stderr, "[ERROR] usage: proof OUT_DIR | proof --check\n");
    return 1;
  }
  for (uint32_t i = 0; i < sizeof kProofs / sizeof kProofs[0]; ++i) {
    char path[1024];
    uint8_t frame[CV_FRAME_BYTES];
    snprintf(path, sizeof path, "%s/%02u-%s.png", argv[1], (unsigned)(i + 1u), kProofs[i].name);
    cvScreenDraw(frame, kProofs[i].screen, kInstalledExample, kProofs[i].connected);
    if (!pngWrite(path, frame)) {
      fprintf(stderr, "[ERROR] cannot write %s\n", path);
      return 1;
    }
    printf("%s\n", path);
  }
  return 0;
}
