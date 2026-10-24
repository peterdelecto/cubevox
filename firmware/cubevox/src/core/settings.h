// User settings: signal level, the input jack, the polish stages and the top switch. Defaults match
// the hardware at power-up.

#pragma once

#include <stdint.h>

enum class OutputLevel : uint8_t { Line, Instrument };
enum class InputJack : uint8_t { Mic, Pedal };  // Mic = relay K103 coil off, pad shorted
enum class TopSwitch : uint8_t { Mute, Bypass };  // what the top-left switch does

struct Settings {
  OutputLevel outputLevel = OutputLevel::Line;
  InputJack inputJack = InputJack::Mic;
  bool polishComp = true;  // POLISH COMP stage
  bool polishEq = true;    // POLISH EQ stage
  TopSwitch topSwitch = TopSwitch::Mute;
};

// INSTRUMENT is about -10 dBV against the 1.27 Vrms LINE reference.
constexpr float kInstrumentTrimDb = -12.0f;

// Output trim applied after the chain.
inline float outputTrimDb(const Settings& s) {
  return s.outputLevel == OutputLevel::Instrument ? kInstrumentTrimDb : 0.0f;
}
