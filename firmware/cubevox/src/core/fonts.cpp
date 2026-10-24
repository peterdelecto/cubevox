#include "core/fonts.h"

#include "cubevox_fonts.h"

namespace {

namespace f = cubevox::fonts;

// The export gives each font its own namespace, so a macro builds the descriptor.
#define CUBEVOX_DESCRIBE(ns, gap)                                                                      \
  FontDesc {                                                                                           \
    f::ns::kWidth, f::ns::kHeight, f::ns::kSpacing, static_cast<uint8_t>(f::ns::kBytesPerGlyph),        \
        &f::ns::kCodes[0], f::ns::kGlyphCount, &f::ns::kGlyphs[0][0], &f::ns::kInkLeft[0],              \
        &f::ns::kInkWidth[0], f::ns::kSpaceAdvance, gap                                                 \
  }

// Blank columns between neighbouring inks in the fonts spaced by ink.
constexpr uint8_t kInkGap = 3;

// The 5x7 font stays on its fixed 6 px pitch, which the menu and update screens are laid out on.
constexpr FontDesc k5x7 = CUBEVOX_DESCRIBE(StylizedHyperlegible5x7, 0);
constexpr FontDesc k9x12 = CUBEVOX_DESCRIBE(StylizedHyperlegible9X12, kInkGap);
constexpr FontDesc k16x24 = CUBEVOX_DESCRIBE(StylizedHyperlegible16X24, kInkGap);

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

int glyphIndex(const FontDesc& font, char c) {
  uint8_t code = static_cast<uint8_t>(c);
  if (code >= 'a' && code <= 'z') code = static_cast<uint8_t>(code - 'a' + 'A');
  return indexOf(font, code);
}

const uint8_t* findGlyph(const FontDesc& font, char c) {
  const int i = glyphIndex(font, c);
  return i < 0 ? nullptr : glyphAt(font, i);
}
