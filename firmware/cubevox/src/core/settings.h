// User settings: signal level, the input jack, CLEANUP EQ, the top switch, the screen
// brightness and the debug readout. Defaults match the hardware at power-up.

#pragma once

#include <stdint.h>

#include "core/brightness.h"

enum class OutputLevel : uint8_t { Line, Mic };
enum class InputJack : uint8_t { Mic, Line };  // Mic = relay K103 coil off, pad shorted
enum class TopSwitch : uint8_t { Mute, Bypass };  // what the top-left switch does

// The CLEANUP EQ row. The values are the saved byte and the order the menu steps through.
enum class CleanupEq : uint8_t {
  Off,     // the stage does not run
  Light,   // the subsonic high-pass and de-esser ahead of slot 1, the dip, presence and trim after the last slot
  Strong,  // Light, plus the 70 Hz high-pass and 6 kHz low-pass ahead of slot 1
};

struct Settings {
  OutputLevel outputLevel = OutputLevel::Line;
  InputJack inputJack = InputJack::Mic;
  CleanupEq cleanupEq = CleanupEq::Off;  // CLEANUP EQ stage, both halves
  TopSwitch topSwitch = TopSwitch::Mute;
  Brightness brightness = Brightness::High;  // the OLED's contrast while awake; saved, never sent over USB
  bool debug = false;  // DEBUG: the voice screen shows the audio load
};

// MIC output is a -30 dB digital trim before the DAC that puts the output near the level of the mic
// the box received, so a mixer mic channel needs no gain change when the box is inserted.
constexpr float kMicTrimDb = -30.0f;

// Output trim applied after the chain.
inline float outputTrimDb(const Settings& s) {
  return s.outputLevel == OutputLevel::Mic ? kMicTrimDb : 0.0f;
}
