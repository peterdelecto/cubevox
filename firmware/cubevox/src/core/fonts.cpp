#include "core/fonts.h"

#include "cubevox_fonts.h"

namespace {

namespace f = cubevox::fonts;

// The export gives each font its own namespace, so a macro builds the descriptor.
#define CUBEVOX_DESCRIBE(ns)                                                         \
  FontDesc {                                                                         \
    f::ns::kWidth, f::ns::kHeight, f::ns::kSpacing,                                  \
        static_cast<uint8_t>(f::ns::kBytesPerGlyph),                                 \
        reinterpret_cast<const char*>(f::ns::kCodes), f::ns::kGlyphCount,            \
        &f::ns::kGlyphs[0][0], true                                                  \
  }

int indexOf(const FontDesc& font, uint8_t code) {
  int lo = 0;
  int hi = font.glyphCount - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const uint8_t at = static_cast<uint8_t>(font.codes[mid]);
    if (at == code) return mid;
    if (at < code) lo = mid + 1; else hi = mid - 1;
  }
  return -1;
}

}  // namespace

const FontDesc& font5x7() {
  static const FontDesc font = CUBEVOX_DESCRIBE(StylizedHyperlegible5x7);
  return font;
}

const FontDesc& font9x12() {
  static const FontDesc font = CUBEVOX_DESCRIBE(StylizedHyperlegible9X12);
  return font;
}

const FontDesc& font16x24() {
  static const FontDesc font = CUBEVOX_DESCRIBE(StylizedHyperlegible16X24);
  return font;
}

const uint8_t* findGlyph(const FontDesc& font, char c) {
  uint8_t code = static_cast<uint8_t>(c);
  const bool lower = code >= 'a' && code <= 'z';
  if (lower && font.singleCase) code = static_cast<uint8_t>(code - 'a' + 'A');
  int i = indexOf(font, code);
  if (i < 0 && lower) i = indexOf(font, static_cast<uint8_t>(code - 'a' + 'A'));
  return i < 0 ? nullptr : font.glyphs + i * font.bytesPerGlyph;
}
