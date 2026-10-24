// User settings: signal level, the input jack, the polish stages and the top switch. Defaults match
// the hardware at power-up.

#pragma once

#include <stdint.h>

enum class OutputLevel : uint8_t { Line, Mic };
enum class InputJack : uint8_t { Mic, Line };  // Mic = relay K103 coil off, pad shorted
enum class TopSwitch : uint8_t { Mute, Bypass };  // what the top-left switch does

struct Settings {
  OutputLevel outputLevel = OutputLevel::Line;
  InputJack inputJack = InputJack::Mic;
  bool polishComp = true;  // POLISH COMP stage
  bool polishEq = true;    // POLISH EQ stage
  TopSwitch topSwitch = TopSwitch::Mute;
};

// MIC output is a -30 dB digital trim before the DAC that puts the output near the level of the mic
// the box received, so a mixer mic channel needs no gain change when the box is inserted.
constexpr float kMicTrimDb = -30.0f;

// Output trim applied after the chain.
inline float outputTrimDb(const Settings& s) {
  return s.outputLevel == OutputLevel::Mic ? kMicTrimDb : 0.0f;
}
