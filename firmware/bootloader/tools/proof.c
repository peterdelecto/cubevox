// Draws every update-mode screen with screens.c and writes OUT_DIR/NN-<state>.png at four times the
// panel's size, white on black like the panel, inside a grey margin that marks the panel's edge.
// The PNG holds stored deflate blocks, so no library is needed. build.sh --proofs runs it.
//
//   proof OUT_DIR

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
    {"done", CvScreenDone, false},
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

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "[ERROR] usage: proof OUT_DIR\n");
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
