// User settings that change signal level or the input jack. Defaults match the hardware at power-up.

#pragma once

#include <stdint.h>

enum class OutputLevel : uint8_t { Line, Instrument };
enum class InputJack : uint8_t { Mic, Pedal };  // Mic = relay K103 coil off, pad shorted

struct Settings {
  OutputLevel output = OutputLevel::Line;
  InputJack inputJack = InputJack::Mic;
};

// INSTRUMENT is about -10 dBV against the 1.27 Vrms LINE reference.
constexpr float kInstrumentTrimDb = -12.0f;

// Output trim applied after the chain.
inline float outputTrimDb(const Settings& s) {
  return s.output == OutputLevel::Instrument ? kInstrumentTrimDb : 0.0f;
}
