// The bootloader's one C++ file. cubevox_fonts.h is a C++ header, so this unit hands its Hyperlegible
// 5x7 table to the C code through font.h. No glyph is copied.

#include "font.h"

#include "cubevox_fonts.h"

namespace {

namespace f = cubevox::fonts;

static_assert(f::StylizedHyperlegible5x7::kBytesPerGlyph <= UINT8_MAX, "5x7 glyph size fits CvFont");

}  // namespace

extern "C" const CvFont cvFont5x7 = {
    f::StylizedHyperlegible5x7::kWidth,         f::StylizedHyperlegible5x7::kHeight,
    f::StylizedHyperlegible5x7::kSpacing,       static_cast<uint8_t>(f::StylizedHyperlegible5x7::kBytesPerGlyph),
    f::StylizedHyperlegible5x7::kGlyphCount,    &f::StylizedHyperlegible5x7::kCodes[0],
    &f::StylizedHyperlegible5x7::kGlyphs[0][0]};
