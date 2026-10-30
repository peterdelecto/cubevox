// Menu glyph proof. Draws every inked glyph of Myrtle 7X12 and the menu's value strings through the
// firmware's own Canvas and fonts, and writes each line as a 1x PGM plus a second PGM that holds only
// the three new glyphs, `>`, `k` and `z`, so the sheet can mark them. menu_glyph_sheet.py composes
// the PNG, with the 4x glyph grid read from the font library. Exploration only; the owner refines the
// drafts in the font editor.
//
// In bash, from the repo root.
//
//   clang++ -std=c++17 -O1 -I firmware/cubevox/src -I tools/font-editor/fonts \
//     tools/oled-proof/menu_glyph_proof.cpp firmware/cubevox/src/core/canvas.cpp \
//     firmware/cubevox/src/core/fonts.cpp -o build/menu_glyph_proof \
//   && mkdir -p build/menu-glyph-proof && build/menu_glyph_proof build/menu-glyph-proof \
//   && python3 -I tools/oled-proof/menu_glyph_sheet.py build/menu-glyph-proof tools/oled-proof/menu-glyph-proof.png

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/canvas.h"
#include "core/fonts.h"

namespace {

constexpr int kPad = 2;
constexpr int kMaxLineWidth = kCanvasWidth - 2 * kPad;
constexpr const char* kNewChars = ">kz";

bool isNew(char c) { return std::strchr(kNewChars, c) != nullptr && c != '\0'; }

bool litAt(const Canvas& c, int x, int y) { return (c.page(y / 8)[x] >> (y % 8)) & 1; }

bool writePgm(const std::string& path, int w, int h, const std::vector<uint8_t>& px) {
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) {
    std::perror(path.c_str());
    return false;
  }
  std::fprintf(file, "P5\n%d %d\n255\n", w, h);
  std::fwrite(px.data(), 1, px.size(), file);
  std::fclose(file);
  return true;
}

// The 7X12 descriptor with every glyph but the new ones blanked and the ink tables kept, so text
// drawn in it lands where the full font puts it and shows only the new glyphs.
struct NewOnly {
  std::vector<uint8_t> glyphs;
  FontDesc desc;

  explicit NewOnly(const FontDesc& base) : glyphs(base.glyphs, base.glyphs + base.glyphCount * base.bytesPerGlyph), desc(base) {
    for (int i = 0; i < base.glyphCount; ++i) {
      if (isNew(static_cast<char>(base.codes[i]))) continue;
      std::memset(&glyphs[static_cast<size_t>(i) * base.bytesPerGlyph], 0, base.bytesPerGlyph);
    }
    desc.glyphs = glyphs.data();
  }
};

// One specimen line, `width` px of text with kPad px around it. Writes the full line and the line
// with only the new glyphs, and returns the ink span of each new glyph as "x0-x1" joined by commas,
// in the line image's own pixels.
bool writeLine(const std::string& dir, const std::string& id, const FontDesc& font, const FontDesc& newOnly,
               const std::string& text, std::string& spans) {
  const int band = font.height + 2 * kPad;
  const int width = Canvas::textWidth(font, text.c_str()) + 2 * kPad;
  if (width > kCanvasWidth) {
    std::fprintf(stderr, "line %s is %d px wide, more than the %d px canvas\n", id.c_str(), width, kCanvasWidth);
    return false;
  }
  Canvas full;
  Canvas only;
  full.drawText(kPad, kPad, font, text.c_str());
  only.drawText(kPad, kPad, newOnly, text.c_str());
  std::vector<uint8_t> pxFull(static_cast<size_t>(width) * band, 0);
  std::vector<uint8_t> pxOnly(static_cast<size_t>(width) * band, 0);
  for (int y = 0; y < band; ++y) {
    for (int x = 0; x < width; ++x) {
      if (litAt(full, x, y)) pxFull[static_cast<size_t>(y) * width + x] = 255;
      if (litAt(only, x, y)) pxOnly[static_cast<size_t>(y) * width + x] = 255;
    }
  }
  spans.clear();
  for (size_t i = 0; i < text.size(); ++i) {
    if (!isNew(text[i])) continue;
    const std::string prefix = text.substr(0, i);
    Canvas scratch;
    const int pen = kPad + scratch.drawText(0, 0, font, prefix.c_str());
    const int index = glyphIndex(font, text[i]);
    const int ink = font.inkWidth[index];
    spans += (spans.empty() ? "" : ",") + std::to_string(pen) + "-" + std::to_string(pen + ink - 1);
  }
  return writePgm(dir + "/glyph-line-" + id + ".pgm", width, band, pxFull) &&
         writePgm(dir + "/glyph-line-" + id + "-new.pgm", width, band, pxOnly);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "build";
  const FontDesc& font = font7x12();
  const NewOnly newOnly(font);

  // The new glyphs must carry ink in the header the firmware compiled.
  for (const char* c = kNewChars; *c; ++c) {
    const int index = glyphIndex(font, *c);
    if (index < 0 || font.inkWidth[index] == 0) {
      std::fprintf(stderr, "7X12 has no ink for '%c'\n", *c);
      return 1;
    }
  }

  // Every inked glyph in code order, split into lines that fit the canvas.
  std::vector<std::string> setLines;
  std::string line;
  std::string blanks;
  for (int i = 0; i < font.glyphCount; ++i) {
    const char c = static_cast<char>(font.codes[i]);
    if (c < '!') continue;
    if (font.inkWidth[i] == 0) {
      blanks += c;
      continue;
    }
    const std::string next = line + c;
    if (Canvas::textWidth(font, next.c_str()) > kMaxLineWidth) {
      setLines.push_back(line);
      line.clear();
    }
    line += c;
  }
  if (!line.empty()) setLines.push_back(line);

  FILE* manifest = std::fopen((dir + "/glyph-lines.txt").c_str(), "w");
  if (!manifest) {
    std::perror("glyph-lines.txt");
    return 1;
  }
  bool ok = true;
  std::string spans;

  // Section A lines: the set. Section B lines: the strings the menu shows.
  for (size_t i = 0; i < setLines.size(); ++i) {
    const std::string id = "set" + std::to_string(i);
    ok = writeLine(dir, id, font, newOnly.desc, setLines[i], spans) && ok;
    std::fprintf(manifest, "set|%s|%d|%s|%s\n", id.c_str(), Canvas::textWidth(font, setLines[i].c_str()), spans.c_str(),
                 setLines[i].c_str());
  }
  const char* const strings[] = {"120 Hz", "12 kHz", "UPDATE FIRMWARE >"};
  for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); ++i) {
    const std::string id = "str" + std::to_string(i);
    ok = writeLine(dir, id, font, newOnly.desc, strings[i], spans) && ok;
    std::fprintf(manifest, "str|%s|%d|%s|%s\n", id.c_str(), Canvas::textWidth(font, strings[i]), spans.c_str(), strings[i]);
    std::printf("%-20s %3d px\n", strings[i], Canvas::textWidth(font, strings[i]));
  }
  std::fprintf(manifest, "blank|-|0||%s\n", blanks.c_str());
  std::fclose(manifest);
  return ok ? 0 : 1;
}
