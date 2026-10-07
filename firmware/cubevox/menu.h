// MENU encoder state machine and the OLED screens.
// Drawing goes through the Display interface: the SH1106 driver when the panel answers on
// I2C2, otherwise a serial-logging stub. Lines are 16 columns, the SH1106 at the 8x8 font.

#pragma once

#include <stdint.h>

#include "slots.h"

// Settings the menu owns. The main loop copies them into ChainParams.
struct MenuSettings {
  int reverbEngine = cv::kReverbSpringB;  // cv::kReverbChasm or cv::kReverbSpringB
  bool outputInstrument = false;           // false = LINE, true = INSTRUMENT
  bool inputMic = false;                   // false = PEDAL, true = MIC (adds kMicTrimDb)
};

// 128x64 text display. The real driver implements the same four calls.
class Display {
 public:
  virtual ~Display() {}
  virtual void clear() = 0;
  virtual void text(int row, const char* line) = 0;  // row 0..7
  virtual void invertRow(int row) = 0;
  virtual void flush() = 0;
};

// Returned when no SH1106 driver is linked: prints each changed screen to Serial.
Display& stubDisplay();

void menuInit(Display& display);

// Call every loop pass. Reads the encoder state, runs the menu, redraws on change.
// `muted` and the last-moved knob feed the idle screen.
void menuPoll(uint32_t nowMs, bool muted);

const MenuSettings& menuSettings();

// True once after a setting changed.
bool menuTakeSettingsChanged();

// Input trim for the PEDAL / MIC choice, applied ahead of the chain.
float menuInputGainDb();

// Output trim for the LINE / INSTRUMENT choice, applied after the chain.
float menuOutputGainDb();
