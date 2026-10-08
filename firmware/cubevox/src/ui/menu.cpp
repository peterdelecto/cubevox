#include "ui/menu.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "board/controls.h"
#include "board/pins.h"
#include "board/platform.h"
#include "core/registry.h"

namespace {

constexpr int kRows = 8;
constexpr int kCols = 16;  // SH1106 at the 8x8 font
constexpr int kCountsPerDetent = 2;  // EC11E: 15 pulses and 30 detents per turn
constexpr uint32_t kClickDebounceMs = 20;
constexpr uint32_t kMenuTimeoutMs = 15000;
constexpr float kInstrumentTrimDb = -12.0f;  // INSTRUMENT is about -10 dBV against the 1.27 Vrms LINE reference
constexpr float kMicTrimDb = 25.7f;          // gives back the 1/4" jack pad, -25.7 dB (hardware spec item 2)

enum class Item : uint8_t { ReverbEngine, Output, InputJack, ChainOrder, UpdateFirmware, Exit, Count };
enum class Mode : uint8_t { Idle, List, ChainView, ConfirmDfu };

constexpr int kItemCount = static_cast<int>(Item::Count);

// Encoder decode. The ISR only updates these.
volatile int gCounts = 0;
volatile uint8_t gQuadState = 0;

Display* gDisplay = nullptr;
MenuSettings gSettings;
bool gSettingsChanged = false;
DebouncedInput gClick;
Mode gMode = Mode::Idle;
int gCursor = 0;
uint32_t gLastActivityMs = 0;
char gFrame[kRows][kCols + 1];
char gShown[kRows][kCols + 1];

void encoderIsr() {
  static const int8_t kStep[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  const uint8_t ab = static_cast<uint8_t>((digitalRead(pins::kEncMenuA) << 1) | digitalRead(pins::kEncMenuB));
  gQuadState = static_cast<uint8_t>(((gQuadState << 2) | ab) & 0x0F);
  gCounts = gCounts + kStep[gQuadState];
}

int takeDetents() {
  noInterrupts();
  const int counts = gCounts;
  const int detents = counts / kCountsPerDetent;
  gCounts = counts - detents * kCountsPerDetent;
  interrupts();
  return detents;
}

void row(int r, const char* text) { snprintf(gFrame[r], sizeof(gFrame[r]), "%s", text); }

const char* itemLabel(Item it) {
  switch (it) {
    case Item::ReverbEngine: return "Reverb";
    case Item::Output: return "Out";
    case Item::InputJack: return "Input";
    case Item::ChainOrder: return "Chain order";
    case Item::UpdateFirmware: return "Update firmware";
    case Item::Exit: return "Exit";
    case Item::Count: break;
  }
  return "";
}

const char* itemValue(Item it) {
  switch (it) {
    case Item::ReverbEngine: return gSettings.reverbEngine == cv::kReverbChasm ? "CHASM" : "SPRING B";
    case Item::Output: return gSettings.outputInstrument ? "INSTRUMENT" : "LINE";
    case Item::InputJack: return gSettings.inputMic ? "MIC" : "PEDAL";
    default: return "";
  }
}

void drawIdle(bool muted) {
  row(0, "cubevox");
  row(1, muted ? "MUTED" : "");
  const LastMoved m = controlsLastMoved();
  if (m.slot < 0) return;
  const Card card = slotCard(m.slot);
  const int knob = m.knobB ? 1 : 0;
  char value[kCols + 1];
  formatKnob(card, knob, controlsKnobValue(m.slot, m.knobB), value, sizeof(value));
  row(3, cardDesc(card).name);
  row(4, cardDesc(card).knob[knob].name);
  row(5, value);
  row(6, controlsToggleOn(m.slot) ? "ON" : "OFF");
}

void drawList() {
  row(0, "MENU");
  for (int i = 0; i < kItemCount; ++i) {
    const Item it = static_cast<Item>(i);
    char line[kCols + 1];
    snprintf(line, sizeof(line), "%s%s %s", i == gCursor ? ">" : " ", itemLabel(it), itemValue(it));
    row(1 + i, line);
  }
}

void drawChainView() {
  row(0, "CHAIN ORDER");
  for (int i = 0; i < kSlotCount; ++i) {
    char line[kCols + 1];
    snprintf(line, sizeof(line), "%d %s", i + 1, cardDesc(slotCard(i)).name);
    row(1 + i, line);
  }
}

void drawConfirmDfu() {
  row(0, "UPDATE FIRMWARE");
  row(2, "Click to reboot");
  row(3, "into DFU mode.");
  row(5, "Turn to cancel.");
}

void redraw(bool muted) {
  memset(gFrame, 0, sizeof(gFrame));
  switch (gMode) {
    case Mode::Idle: drawIdle(muted); break;
    case Mode::List: drawList(); break;
    case Mode::ChainView: drawChainView(); break;
    case Mode::ConfirmDfu: drawConfirmDfu(); break;
  }
  if (memcmp(gFrame, gShown, sizeof(gFrame)) == 0) return;
  memcpy(gShown, gFrame, sizeof(gFrame));
  gDisplay->clear();
  for (int r = 0; r < kRows; ++r) {
    if (gFrame[r][0]) gDisplay->text(r, gFrame[r]);
  }
  if (gMode == Mode::List) gDisplay->invertRow(1 + gCursor);
  gDisplay->flush();
}

void changeSetting() { gSettingsChanged = true; }

void activateItem(Item it) {
  switch (it) {
    case Item::ReverbEngine:
      gSettings.reverbEngine = gSettings.reverbEngine == cv::kReverbSpringB ? cv::kReverbChasm : cv::kReverbSpringB;
      changeSetting();
      break;
    case Item::Output: gSettings.outputInstrument = !gSettings.outputInstrument; changeSetting(); break;
    case Item::InputJack: gSettings.inputMic = !gSettings.inputMic; changeSetting(); break;
    case Item::ChainOrder: gMode = Mode::ChainView; break;
    case Item::UpdateFirmware: gMode = Mode::ConfirmDfu; break;
    case Item::Exit: gMode = Mode::Idle; break;
    case Item::Count: break;
  }
}

void onClick() {
  switch (gMode) {
    case Mode::Idle: gMode = Mode::List; gCursor = 0; break;
    case Mode::List: activateItem(static_cast<Item>(gCursor)); break;
    case Mode::ChainView: gMode = Mode::List; break;
    case Mode::ConfirmDfu: platformEnterDfu(); break;  // does not return
  }
}

void onTurn(int detents) {
  if (gMode == Mode::List) {
    gCursor = (gCursor + detents % kItemCount + kItemCount) % kItemCount;
  } else if (gMode == Mode::ConfirmDfu) {
    gMode = Mode::List;
  }
}

}  // namespace

class StubDisplay : public Display {
 public:
  void clear() override { memset(rows_, 0, sizeof(rows_)); inverted_ = -1; }
  void text(int r, const char* line) override { snprintf(rows_[r], sizeof(rows_[r]), "%.*s", kCols, line); }
  void invertRow(int r) override { inverted_ = r; }
  void flush() override {
    Serial.println("[oled] ----------------------");
    for (int r = 0; r < kRows; ++r) {
      Serial.print(r == inverted_ ? "[oled] *" : "[oled]  ");
      Serial.println(rows_[r]);
    }
  }

 private:
  char rows_[kRows][kCols + 1] = {};
  int inverted_ = -1;
};

Display& stubDisplay() {
  static StubDisplay display;
  return display;
}

void menuInit(Display& display) {
  gDisplay = &display;
  pinMode(pins::kEncMenuA, INPUT_PULLUP);
  pinMode(pins::kEncMenuB, INPUT_PULLUP);
  pinMode(pins::kEncMenuSw, INPUT_PULLUP);
  gClick.reset(digitalRead(pins::kEncMenuSw) == HIGH);
  gQuadState = static_cast<uint8_t>((digitalRead(pins::kEncMenuA) << 1) | digitalRead(pins::kEncMenuB));
  attachInterrupt(digitalPinToInterrupt(pins::kEncMenuA), encoderIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(pins::kEncMenuB), encoderIsr, CHANGE);
}

void menuPoll(uint32_t nowMs, bool muted) {
  const int detents = takeDetents();
  const bool pressed = gClick.update(digitalRead(pins::kEncMenuSw) == HIGH, nowMs, kClickDebounceMs) &&
                       !gClick.level();
  if (detents != 0 || pressed) gLastActivityMs = nowMs;
  if (detents != 0) onTurn(detents);
  if (pressed) onClick();
  if (gMode != Mode::Idle && nowMs - gLastActivityMs > kMenuTimeoutMs) gMode = Mode::Idle;
  redraw(muted);
}

const MenuSettings& menuSettings() { return gSettings; }

bool menuTakeSettingsChanged() {
  const bool changed = gSettingsChanged;
  gSettingsChanged = false;
  return changed;
}

float menuOutputGainDb() { return gSettings.outputInstrument ? kInstrumentTrimDb : 0.0f; }

float menuInputGainDb() { return gSettings.inputMic ? kMicTrimDb : 0.0f; }
