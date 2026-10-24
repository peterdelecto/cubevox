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
  std::fclose(file);
  if (got != want.size()) {
    std::printf("FAIL golden %s: %zu bytes, want %zu\n", name, got, want.size());
    return false;
  }
  for (int p = 0; p < kCanvasPages; ++p) {
    if (std::memcmp(canvas.page(p), want.data() + p * kCanvasWidth, kCanvasWidth) == 0) continue;
    writePgm(canvas, (std::string("oled-mismatch-") + name + ".pgm").c_str());
    return false;
  }
  return true;
}
