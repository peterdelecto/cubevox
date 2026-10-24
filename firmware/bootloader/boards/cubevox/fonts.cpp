// The bootloader's one C++ file. cubevox_fonts.h is a C++ header, so this unit hands its Hyperlegible
// 5x7 and 9X12 tables to the C code through font.h. No glyph is copied.

#include "font.h"

#include "cubevox_fonts.h"

namespace {

namespace f = cubevox::fonts;

static_assert(f::StylizedHyperlegible5x7::kBytesPerGlyph <= UINT8_MAX, "5x7 glyph size fits CvFont");
static_assert(f::StylizedHyperlegible9X12::kBytesPerGlyph <= UINT8_MAX, "9X12 glyph size fits CvFont");

}  // namespace

// The export gives each font its own namespace, so a macro builds the descriptor.
#define CV_DESCRIBE(ns)                                                                          \
  {                                                                                              \
    f::ns::kWidth, f::ns::kHeight, f::ns::kSpacing, static_cast<uint8_t>(f::ns::kBytesPerGlyph), \
        f::ns::kGlyphCount, &f::ns::kCodes[0], &f::ns::kGlyphs[0][0]                             \
  }

extern "C" const CvFont cvFont5x7 = CV_DESCRIBE(StylizedHyperlegible5x7);
extern "C" const CvFont cvFont9x12 = CV_DESCRIBE(StylizedHyperlegible9X12);
