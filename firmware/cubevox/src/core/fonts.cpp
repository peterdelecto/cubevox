#include "core/fonts.h"

#include "cubevox_fonts.h"

namespace {

namespace f = cubevox::fonts;

// The export gives each font its own namespace, so a macro builds the descriptor.
#define CUBEVOX_DESCRIBE(ns)                                                                           \
  FontDesc {                                                                                           \
    f::ns::kWidth, f::ns::kHeight, f::ns::kSpacing, static_cast<uint8_t>(f::ns::kBytesPerGlyph),        \
        &f::ns::kCodes[0], f::ns::kGlyphCount, &f::ns::kGlyphs[0][0]                                    \
  }

constexpr FontDesc k5x7 = CUBEVOX_DESCRIBE(StylizedHyperlegible5x7);
constexpr FontDesc k9x12 = CUBEVOX_DESCRIBE(StylizedHyperlegible9X12);
constexpr FontDesc k16x24 = CUBEVOX_DESCRIBE(StylizedHyperlegible16X24);

static_assert(kFont5x7Advance == k5x7.width + k5x7.spacing, "kFont5x7Advance must match the export");

int indexOf(const FontDesc& font, uint8_t code) {
  int lo = 0;
  int hi = font.glyphCount - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const uint8_t at = font.codes[mid];
    if (at == code) return mid;
    if (at < code) lo = mid + 1; else hi = mid - 1;
  }
  return -1;
}

}  // namespace

const FontDesc& font5x7() { return k5x7; }

const FontDesc& font9x12() { return k9x12; }

const FontDesc& font16x24() { return k16x24; }

const uint8_t* findGlyph(const FontDesc& font, char c) {
  uint8_t code = static_cast<uint8_t>(c);
  if (code >= 'a' && code <= 'z') code = static_cast<uint8_t>(code - 'a' + 'A');
  const int i = indexOf(font, code);
  return i < 0 ? nullptr : font.glyphs + i * font.bytesPerGlyph;
}
