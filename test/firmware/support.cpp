#include "support.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

int gFailures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++gFailures;
  }
}

Card card(int i) { return static_cast<Card>(i); }

namespace {

// The standard Bayer 8 x 8 thresholds, indexed [y & 7][x & 7]. The dissolve checks hold the canvas's
// own masks against this definition.
constexpr uint8_t kBayerRef[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},    {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22}, {3, 35, 11, 43, 1, 33, 9, 41},    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},   {63, 31, 55, 23, 61, 29, 53, 21},
};

}  // namespace

bool bayerLit(int x, int y, int level) { return kBayerRef[y & 7][x & 7] < level; }

void clearAllDirty(Canvas& canvas) {
  for (int p = 0; p < kCanvasPages; ++p) canvas.clearDirty(p);
}

void writePgm(const Canvas& canvas, const char* name) {
  constexpr int kScale = 4;
  const std::string path = std::string(CUBEVOX_OUT_DIR) + "/" + name;
  std::FILE* file = std::fopen(path.c_str(), "wb");
  check(file != nullptr, "pgm: output file opens");
  if (!file) return;
  std::fprintf(file, "P5\n%d %d\n255\n", kCanvasWidth * kScale, kCanvasHeight * kScale);
  for (int y = 0; y < kCanvasHeight * kScale; ++y) {
    const int py = y / kScale;
    for (int x = 0; x < kCanvasWidth * kScale; ++x) {
      const bool on = (canvas.page(py / 8)[x / kScale] >> (py % 8)) & 1;
      std::fputc(on ? 0xFF : 0x00, file);
    }
  }
  std::fclose(file);
}

bool matchesGolden(const Canvas& canvas, const char* name) {
  std::array<uint8_t, kCanvasWidth * kCanvasPages> want{};
  const std::string path = std::string(CUBEVOX_GOLDEN_DIR) + "/" + name + ".bin";
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) {
    std::printf("FAIL golden %s: cannot open %s\n", name, path.c_str());
    return false;
  }
  const size_t got = std::fread(want.data(), 1, want.size(), file);
  uint8_t extra = 0;
  const bool longer = got == want.size() && std::fread(&extra, 1, 1, file) == 1;
  std::fclose(file);
  if (got != want.size() || longer) {
    std::printf("FAIL golden %s: %s, want %zu bytes\n", name, longer ? "more bytes" : "too few bytes", want.size());
    return false;
  }
  for (int p = 0; p < kCanvasPages; ++p) {
    if (std::memcmp(canvas.page(p), want.data() + p * kCanvasWidth, kCanvasWidth) == 0) continue;
    writePgm(canvas, (std::string("oled-mismatch-") + name + ".pgm").c_str());
    return false;
  }
  return true;
}
