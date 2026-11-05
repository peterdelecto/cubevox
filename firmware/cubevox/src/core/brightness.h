// How bright the OLED is. The OLED BRIGHT menu item steps through four levels, each one an SH1106
// contrast value, and a dimmed screen uses a fifth, lower one. The menu text, the saved record and
// the dim logic all read this table.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>

enum class Brightness : uint8_t { Low, Mid, High, Max };

struct BrightnessLevel {
  const char* name;  // the menu value
  uint8_t contrast;  // the SH1106 contrast register, 0 to 255
};

constexpr std::array<BrightnessLevel, 4> kBrightnessLevels = {{
    {"LOW", 48},
    {"MID", 128},
    {"HIGH", 207},  // the default, and the contrast the panel's init sequence sets
    {"MAX", 255},
}};

// The contrast of a screen left idle.
constexpr uint8_t kDimmedContrast = 8;

constexpr const BrightnessLevel& brightnessLevel(Brightness b) { return kBrightnessLevels[static_cast<size_t>(b)]; }

// The level a click on the OLED BRIGHT item moves to: the next one, and Low after Max.
constexpr Brightness nextBrightness(Brightness b) {
  return static_cast<size_t>(b) + 1 < kBrightnessLevels.size() ? static_cast<Brightness>(static_cast<size_t>(b) + 1)
                                                                : Brightness::Low;
}
